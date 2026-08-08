#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <manyforge_core/types/catalog.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/capability_definition_codec.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/command_schema_codec.hpp>
#include <manyforge_deployment/component_type_codec.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/control_interface_contract_codec.hpp>
#include <manyforge_deployment/controller_topology_codec.hpp>
#include <manyforge_deployment/deployment_artifact_codec.hpp>
#include <manyforge_deployment/provider_manifest_codec.hpp>
#include <manyforge_deployment/runtime_profile_codec.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>
#include <manyforge_deployment/stop_evidence_policy_codec.hpp>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

struct Options final {
  std::filesystem::path workspace_root;
};

struct Scenario final {
  std::string_view source;
  std::string_view family_id;
  bool assistant_enabled;
};

constexpr std::array<Scenario, 7U> kScenarios{{
    {"examples/assistant_modes_scene_authoring.deployment.yaml", "ur10e_robotiq_2f_140", true},
    {"examples/lite6_lite_gripper_pick_and_place.deployment.yaml", "lite6_lite_gripper", false},
    {"examples/pick_and_place.deployment.yaml", "ur5_roboplan", false},
    {"examples/pick_and_place_ur10e_robotiq.deployment.yaml", "ur10e_robotiq_2f_140", false},
    {"examples/semantic_shape_pick.deployment.yaml", "ur5_roboplan", true},
    {"examples/vision_guided_bin_pick.deployment.yaml", "ur5_roboplan", true},
    {"examples/vision_guided_pose_pick.deployment.yaml", "ur5_roboplan", true},
}};

[[nodiscard]] Options parse_options(int argc, char** argv) {
  if (argc != 4 || std::string_view(argv[1]) != "--check" ||
      std::string_view(argv[2]) != "--workspace-root" || std::string_view(argv[3]).empty()) {
    throw std::invalid_argument(
        "usage: manyforge_maintained_deployment_occurrences --check "
        "--workspace-root <manyforge-root>");
  }
  return {std::filesystem::path(argv[3])};
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
  if (std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path)) {
    throw std::invalid_argument("maintained source is not one plain file: " + path.string());
  }
  const auto size = std::filesystem::file_size(path);
  if (size > deployment::kMaxArtifactEncodedBytes) {
    throw std::invalid_argument("maintained source exceeds the artifact bound: " + path.string());
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot open maintained source: " + path.string());
  }
  std::string bytes(static_cast<std::size_t>(size), '\0');
  stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!stream && !bytes.empty()) {
    throw std::runtime_error("cannot read maintained source: " + path.string());
  }
  return bytes;
}

[[nodiscard]] const std::string& require_string_member(const deployment::CanonicalValue& root,
                                                       std::string_view name) {
  const auto* value = root.find_object_member(name);
  if (value == nullptr) {
    throw std::invalid_argument("catalog source is missing " + std::string(name));
  }
  const auto* text = std::get_if<std::string>(&value->storage());
  if (text == nullptr) {
    throw std::invalid_argument("catalog source member is not text: " + std::string(name));
  }
  return *text;
}

[[nodiscard]] std::vector<std::filesystem::path> source_files(
    const std::filesystem::path& directory, std::size_t expected_count) {
  if (std::filesystem::is_symlink(directory) || !std::filesystem::is_directory(directory)) {
    throw std::invalid_argument("maintained catalog directory is unavailable: " +
                                directory.string());
  }
  std::vector<std::filesystem::path> result;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().extension() == ".json") {
      result.push_back(entry.path());
    }
  }
  std::sort(result.begin(), result.end());
  if (result.size() != expected_count) {
    throw std::invalid_argument("maintained catalog directory has the wrong exact source count: " +
                                directory.string());
  }
  return result;
}

template <typename Record>
[[nodiscard]] Record make_reference(std::string_view bytes) {
  constexpr core::CatalogRecordKind kind = Record::kRecordKind;
  const auto* descriptor = core::catalog_record_type_descriptor(kind);
  if (descriptor == nullptr) {
    throw std::logic_error("catalog record kind has no descriptor");
  }
  const auto* schema =
      deployment::build_qualified_closed_schema_registry().find_catalog_schema_family(kind);
  if (schema == nullptr ||
      schema->qualification() != deployment::CatalogSchemaQualification::kQualified ||
      schema->compiled_schema() == nullptr) {
    throw std::logic_error("catalog record schema is not build-qualified");
  }

  const auto parsed = deployment::parse_strict_json_model_yaml(bytes);
  const std::string& record_id = require_string_member(parsed.root(), descriptor->own_id_field);
  const std::string& record_revision =
      require_string_member(parsed.root(), descriptor->own_revision_field);
  if (!record_revision.starts_with("sha256:") || record_revision.size() != 71U) {
    throw std::invalid_argument("catalog source revision is not a SHA-256 digest");
  }
  const core::ContentDigest content_digest(deployment::sha256_digest(bytes));
  const core::ContentAddressedStoreKey store_key("catalog/maintained/" +
                                                 std::string(core::to_string(kind)) + "/" +
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

using CatalogKey = std::tuple<std::uint8_t, std::string, std::string>;

[[nodiscard]] CatalogKey catalog_key(const core::CatalogRecordRefDTO& record) {
  return {static_cast<std::uint8_t>(core::catalog_record_kind(record)),
          core::catalog_record_id_value(record), core::catalog_record_revision_value(record)};
}

struct QualifiedCatalogClosure final {
  std::vector<deployment::QualifiedCapabilityDefinition> capabilities;
  std::vector<deployment::QualifiedRuntimeProfile> runtime_profiles;
  std::map<CatalogKey, core::CatalogRecordRefDTO> records;
};

void insert_reference(QualifiedCatalogClosure& closure, core::CatalogRecordRefDTO reference) {
  if (!closure.records.emplace(catalog_key(reference), reference).second) {
    throw std::invalid_argument("maintained catalog contains a duplicate typed referent");
  }
}

[[nodiscard]] QualifiedCatalogClosure qualify_catalog(const std::filesystem::path& root) {
  const auto catalog_root = root / "manyforge_deployment/resources/maintained/catalog";
  QualifiedCatalogClosure result;

  for (const auto& path : source_files(catalog_root / "capability_definition", 15U)) {
    const std::string bytes = read_file(path);
    const auto reference = make_reference<core::CapabilityDefinitionCatalogRecordRefDTO>(bytes);
    result.capabilities.push_back(deployment::resolve_and_qualify_capability_definition(
        reference, reference.content_addressed_store_key, bytes));
    insert_reference(result, reference);
  }

  std::vector<deployment::QualifiedCapabilityDefinition> target_capabilities;
  for (const auto& capability : result.capabilities) {
    if (capability.value().scope == core::CapabilityScope::kTargetBinding) {
      target_capabilities.push_back(capability);
    }
  }
  if (target_capabilities.size() != 13U) {
    throw std::logic_error("maintained component closure has the wrong target-capability count");
  }
  for (const auto& path : source_files(catalog_root / "component_type", 3U)) {
    const std::string bytes = read_file(path);
    const auto reference = make_reference<core::ComponentTypeCatalogRecordRefDTO>(bytes);
    (void)deployment::resolve_and_qualify_component_type(
        reference, reference.content_addressed_store_key, bytes, target_capabilities);
    insert_reference(result, reference);
  }

  std::vector<deployment::QualifiedControlInterfaceContract> contracts;
  for (const auto& path : source_files(catalog_root / "control_interface_contract", 2U)) {
    const std::string bytes = read_file(path);
    const auto reference = make_reference<core::ControlInterfaceContractCatalogRecordRefDTO>(bytes);
    contracts.push_back(deployment::resolve_and_qualify_control_interface_contract(
        reference, reference.content_addressed_store_key, bytes));
    insert_reference(result, reference);
  }

  for (const auto& path : source_files(catalog_root / "controller_topology", 2U)) {
    const std::string bytes = read_file(path);
    const auto reference = make_reference<core::ControllerTopologyCatalogRecordRefDTO>(bytes);
    const std::string expected_contract_id =
        reference.record_id.value().find("_arm_") != std::string::npos
            ? "manyforge.native_arm_position"
            : "manyforge.native_gripper_position";
    std::vector<deployment::QualifiedControlInterfaceContract> selected;
    std::copy_if(contracts.begin(), contracts.end(), std::back_inserter(selected),
                 [&expected_contract_id](const auto& contract) {
                   return contract.value().control_interface_contract_id.value() ==
                          expected_contract_id;
                 });
    if (selected.size() != 1U) {
      throw std::logic_error("maintained topology cannot select its exact contract closure");
    }
    (void)deployment::resolve_and_qualify_controller_topology(
        reference, reference.content_addressed_store_key, bytes, selected);
    insert_reference(result, reference);
  }

  for (const auto& path : source_files(catalog_root / "provider_manifest", 4U)) {
    const std::string bytes = read_file(path);
    const auto reference = make_reference<core::ProviderManifestCatalogRecordRefDTO>(bytes);
    (void)deployment::resolve_and_qualify_provider_manifest(
        reference, reference.content_addressed_store_key, bytes);
    insert_reference(result, reference);
  }

  for (const auto& path : source_files(catalog_root / "runtime_profile", 1U)) {
    const std::string bytes = read_file(path);
    const auto reference = make_reference<core::RuntimeProfileCatalogRecordRefDTO>(bytes);
    result.runtime_profiles.push_back(deployment::resolve_and_qualify_runtime_profile(
        reference, reference.content_addressed_store_key, bytes));
    insert_reference(result, reference);
  }

  for (const auto& path : source_files(catalog_root / "stop_evidence_policy", 1U)) {
    const std::string bytes = read_file(path);
    const auto reference = make_reference<core::StopEvidencePolicyCatalogRecordRefDTO>(bytes);
    (void)deployment::resolve_and_qualify_stop_evidence_policy(
        reference, reference.content_addressed_store_key, bytes);
    insert_reference(result, reference);
  }

  const std::filesystem::path command_path = root /
                                             "manyforge_deployment/resources/catalog/"
                                             "gripper.command_position.v1.yaml";
  const std::string command_bytes = read_file(command_path);
  const auto command_reference =
      make_reference<core::CommandSchemaCatalogRecordRefDTO>(command_bytes);
  (void)deployment::resolve_and_qualify_command_schema(
      command_reference, command_reference.content_addressed_store_key, command_bytes);
  insert_reference(result, command_reference);

  for (const auto& path :
       source_files(root / "manyforge_deployment/resources/maintained/model_assets", 3U)) {
    const std::string bytes = read_file(path);
    insert_reference(result, make_reference<core::ModelAssetCatalogRecordRefDTO>(bytes));
  }
  if (result.records.size() != 32U || result.runtime_profiles.size() != 1U) {
    throw std::logic_error("maintained exact catalog closure is incomplete");
  }
  return result;
}

void verify_scenarios(const std::filesystem::path& root, const QualifiedCatalogClosure& catalog) {
  const std::string overlay_bytes = read_file(
      root / "manyforge_deployment/resources/maintained/site_overlays/"
             "manyforge_local_examples.site_overlay.json");
  std::map<std::string, std::pair<std::string, std::string>, std::less<>> family_identities;
  for (const auto& scenario : kScenarios) {
    const std::string bytes = read_file(root / scenario.source);
    const auto verified = deployment::verify_deployment_artifact_revision(
        deployment::parse_strict_json_model_yaml(bytes), catalog.runtime_profiles);
    if (verified.assistant_settings().enabled() != scenario.assistant_enabled) {
      throw std::invalid_argument("maintained scenario has the wrong assistant disposition: " +
                                  std::string(scenario.source));
    }
    const auto& applicability = verified.applicability_inputs().value();
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
    const std::size_t expected_catalog_count = scenario.assistant_enabled ? 28U : 24U;
    if (verified.catalog_records().size() != expected_catalog_count) {
      throw std::invalid_argument("maintained scenario has the wrong exact catalog closure: " +
                                  std::string(scenario.source));
    }
    for (const auto& reference : verified.catalog_records()) {
      const auto expected = catalog.records.find(catalog_key(reference));
      if (expected == catalog.records.end() || expected->second != reference) {
        throw std::invalid_argument(
            "maintained scenario selects an unqualified catalog referent: " +
            std::string(scenario.source));
      }
    }

    const auto& graph = verified.graph().graph();
    if (graph.controllable_components.size() != 1U || graph.model_instances.size() != 1U) {
      throw std::invalid_argument(
          "maintained scenario must contain one direct component occurrence: " +
          std::string(scenario.source));
    }
    const auto& component = graph.controllable_components.front();
    const auto& model = graph.model_instances.front();
    const std::string expected_component_id = std::string(scenario.family_id) + ".component";
    if (component.component_type_id.value() != expected_component_id ||
        model.model_asset_id.value() != scenario.family_id) {
      throw std::invalid_argument("maintained scenario selects the wrong shared family closure: " +
                                  std::string(scenario.source));
    }
    const auto identity =
        std::pair{component.component_type_revision.value(), model.model_revision.value()};
    const auto [position, inserted] =
        family_identities.emplace(std::string(scenario.family_id), identity);
    if (!inserted && position->second != identity) {
      throw std::invalid_argument(
          "same-family scenarios select different component/model revisions");
    }
  }
  if (family_identities.size() != 3U) {
    throw std::logic_error("maintained scenarios do not cover three families");
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    const auto root = std::filesystem::canonical(options.workspace_root);
    const QualifiedCatalogClosure catalog = qualify_catalog(root);
    verify_scenarios(root, catalog);
    std::cout << "7 deployments, 3 shared families, and 32 exact catalog "
                 "referents qualified\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "manyforge_maintained_deployment_occurrences: " << error.what() << '\n';
    return 2;
  }
}
