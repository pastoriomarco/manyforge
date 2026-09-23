#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_deployment/activation_preparation_request.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/prepared_activation_input.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/runtime_program_binding.hpp>
#include <manyforge_deployment/scenario_program_admission.hpp>

#include "activation_component_profile_internal.hpp"
#include "build_manifest_internal.hpp"
#include "canonical_empty_admission_internal.hpp"
#include "activation_deployment_intent_internal.hpp"
#include "activation_model_source_closure_internal.hpp"
#include "activation_prepared_model_closure_internal.hpp"
#include "activation_prepared_model_internal.hpp"
#include "prepared_activation_materialization_internal.hpp"

namespace {

namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

struct Options final {
  std::filesystem::path input;
  std::filesystem::path capability_root;
  std::filesystem::path activation_root;
  bool publish_canonical_empty{false};
};

class PreparedCatalogSource final
    : public deployment::ICatalogArtifactSource {
 public:
  explicit PreparedCatalogSource(
      const std::vector<deployment::detail::PreparedActivationArtifact> &values)
      : values_(values) {}

  [[nodiscard]] std::optional<std::string> get_verified(
      const core::ContentAddressedStoreKey &key,
      const core::ContentDigest &digest,
      std::optional<std::uint64_t> size) const override {
    const auto found = std::find_if(values_.begin(), values_.end(),
                                    [&](const auto &value) {
      return value.content_addressed_store_key == key;
    });
    if (found == values_.end() || found->content_digest != digest ||
        (size.has_value() && found->bytes.size() != *size) ||
        deployment::sha256_digest(found->bytes) != digest.value()) {
      return std::nullopt;
    }
    return found->bytes;
  }

 private:
  const std::vector<deployment::detail::PreparedActivationArtifact> &values_;
};

[[nodiscard]] Options parse_options(int argc, char **argv) {
  Options result;
  for (int index = 1; index < argc;) {
    const std::string_view option(argv[index]);
    if (option == "--publish-canonical-empty" &&
        !result.publish_canonical_empty) {
      result.publish_canonical_empty = true;
      ++index;
      continue;
    }
    if (index + 1 >= argc) {
      throw std::invalid_argument("preparation option has no value");
    }
    const std::string value(argv[index + 1]);
    if (option == "--input" && result.input.empty()) {
      result.input = value;
    } else if (option == "--capability-root" &&
               result.capability_root.empty()) {
      result.capability_root = value;
    } else if (option == "--activation-root" &&
               result.activation_root.empty()) {
      result.activation_root = value;
    } else {
      throw std::invalid_argument("invalid or repeated preparation option");
    }
    index += 2;
  }
  if (result.input.empty() || result.capability_root.empty() ||
      result.activation_root.empty()) {
    throw std::invalid_argument(
        "usage: manyforge_prepare_activation --input <request> "
        "--capability-root <root> --activation-root <root> "
        "[--publish-canonical-empty]");
  }
  return result;
}

void materialize_activation(
    const deployment::PreparedActivationInput &prepared,
    const std::filesystem::path &activation_root) {
  const auto &closure = prepared.closure();
  {
    deployment::FilesystemRawArtifactStore store(activation_root);
    for (const auto &artifact : closure.raw_artifacts) {
      const auto stored = store.put_if_absent(artifact.bytes);
      if (stored.content_addressed_store_key !=
              artifact.content_addressed_store_key ||
          stored.content_digest != artifact.content_digest ||
          stored.byte_size != artifact.bytes.size()) {
        throw std::runtime_error(
            "prepared raw artifact materialized with another identity");
      }
    }
  }
  {
    deployment::FilesystemCatalogArtifactStore store(activation_root);
    for (const auto &artifact : closure.catalog_artifacts) {
      const auto stored = store.put_if_absent(
          artifact.content_addressed_store_key, artifact.content_digest,
          artifact.bytes);
      if (stored.content_addressed_store_key !=
              artifact.content_addressed_store_key ||
          stored.content_digest != artifact.content_digest ||
          stored.byte_size != artifact.bytes.size()) {
        throw std::runtime_error(
            "prepared catalog artifact materialized with another identity");
      }
    }
  }
  {
    deployment::FilesystemRetainedBundleRepository store(activation_root);
    static_cast<void>(store.put_if_absent(closure.retained_bundle));
  }
  {
    deployment::FilesystemProgramRepository store(activation_root);
    static_cast<void>(
        store.put_if_absent(closure.program, *closure.retained_bundle));
  }
  {
    deployment::FilesystemProgramBindingRepository store(activation_root);
    static_cast<void>(store.put_if_absent(
        closure.program_binding, *closure.retained_bundle,
        closure.retained_model_map));
  }
}

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  if (std::filesystem::is_symlink(path) ||
      !std::filesystem::is_regular_file(path)) {
    throw std::invalid_argument("preparation input is not one plain file");
  }
  const auto size = std::filesystem::file_size(path);
  if (size == 0U || size > deployment::kMaxActivationPreparationRequestBytes) {
    throw std::invalid_argument("preparation input violates its byte bound");
  }
  std::ifstream stream(path, std::ios::binary);
  std::string bytes(static_cast<std::size_t>(size), '\0');
  stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!stream || stream.peek() != std::char_traits<char>::eof()) {
    throw std::runtime_error("preparation input changed during capture");
  }
  return bytes;
}

[[nodiscard]] const deployment::CanonicalValue &
member(const deployment::CanonicalValue &value, std::string_view name,
       std::string_view label) {
  const auto *result = value.find_object_member(name);
  if (result == nullptr) {
    throw std::invalid_argument(std::string(label) + " is missing " +
                                std::string(name));
  }
  return *result;
}

[[nodiscard]] const std::string &text(const deployment::CanonicalValue &value,
                                      std::string_view label) {
  const auto *result = std::get_if<std::string>(&value.storage());
  if (result == nullptr || result->empty()) {
    throw std::invalid_argument(std::string(label) + " must be non-empty text");
  }
  return *result;
}

[[nodiscard]] const deployment::CanonicalValue::Object &
object(const deployment::CanonicalValue &value, std::string_view label) {
  const auto *result =
      std::get_if<deployment::CanonicalValue::Object>(&value.storage());
  if (result == nullptr) {
    throw std::invalid_argument(std::string(label) + " must be an object");
  }
  return *result;
}

[[nodiscard]] const deployment::CanonicalValue::Array &
array(const deployment::CanonicalValue &value, std::string_view label) {
  const auto *result =
      std::get_if<deployment::CanonicalValue::Array>(&value.storage());
  if (result == nullptr) {
    throw std::invalid_argument(std::string(label) + " must be an array");
  }
  return *result;
}

void require_exact_fields(const deployment::CanonicalValue &value,
                          std::initializer_list<std::string_view> expected,
                          std::string_view label) {
  const auto &members = object(value, label);
  std::set<std::string, std::less<>> actual;
  for (const auto &[name, _] : members) {
    actual.insert(name);
  }
  const std::set<std::string, std::less<>> wanted(expected.begin(),
                                                  expected.end());
  if (actual != wanted) {
    for (const auto &name : wanted) {
      if (!actual.contains(name)) {
        throw std::invalid_argument(std::string(label) + " is missing " + name);
      }
    }
    for (const auto &name : actual) {
      if (!wanted.contains(name)) {
        throw std::invalid_argument(std::string(label) +
                                    " has unexpected field " + name);
      }
    }
    throw std::logic_error("preparation field comparison is inconsistent");
  }
}

[[nodiscard]] core::ProgramRoleAssignmentDTO
parse_assignment(const deployment::CanonicalValue &value) {
  require_exact_fields(value, {"component_roles", "target_roles"},
                       "program_role_assignment");
  core::ProgramRoleAssignmentDTO result;
  std::set<std::string, std::less<>> component_roles;
  for (const auto &row :
       array(member(value, "component_roles", "program_role_assignment"),
             "program_role_assignment.component_roles")) {
    require_exact_fields(row,
                         {"component_role_id", "controllable_component_id"},
                         "component role assignment");
    const auto &role = text(member(row, "component_role_id", "component role"),
                            "component_role_id");
    const auto &component =
        text(member(row, "controllable_component_id", "component role"),
             "controllable_component_id");
    if (!component_roles.insert(role).second) {
      throw std::invalid_argument("duplicate component role assignment");
    }
    result.component_roles.push_back(
        {core::ComponentRoleId(role),
         core::ControllableComponentId(component)});
  }
  std::set<std::string, std::less<>> target_roles;
  for (const auto &row :
       array(member(value, "target_roles", "program_role_assignment"),
             "program_role_assignment.target_roles")) {
    require_exact_fields(row, {"target_role_id", "command_target_id"},
                         "target role assignment");
    const auto &role =
        text(member(row, "target_role_id", "target role"), "target_role_id");
    const auto &target = text(member(row, "command_target_id", "target role"),
                              "command_target_id");
    if (!target_roles.insert(role).second) {
      throw std::invalid_argument("duplicate target role assignment");
    }
    result.target_roles.push_back(
        {core::TargetRoleId(role), core::CommandTargetId(target)});
  }
  return result;
}

[[nodiscard]] deployment::ScenarioAssistantMode
assistant_mode(const deployment::CanonicalValue &program) {
  const auto &compatibility = member(program, "compatibility", "Program");
  const auto &mode = text(member(compatibility, "assistant", "compatibility"),
                          "compatibility.assistant");
  if (mode == "required")
    return deployment::ScenarioAssistantMode::kRequired;
  if (mode == "optional")
    return deployment::ScenarioAssistantMode::kOptional;
  if (mode == "forbidden")
    return deployment::ScenarioAssistantMode::kForbidden;
  throw std::invalid_argument(
      "Program has an unsupported assistant disposition");
}

[[nodiscard]] std::string_view
assistant_mode_text(deployment::ScenarioAssistantMode value) {
  switch (value) {
  case deployment::ScenarioAssistantMode::kRequired:
    return "required";
  case deployment::ScenarioAssistantMode::kOptional:
    return "optional";
  case deployment::ScenarioAssistantMode::kForbidden:
    return "forbidden";
  }
  throw std::logic_error("unknown assistant mode");
}

[[nodiscard]] std::string base64_encode(const std::string_view bytes) {
  static constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve(((bytes.size() + 2U) / 3U) * 4U);
  for (std::size_t offset = 0U; offset < bytes.size(); offset += 3U) {
    const auto first = static_cast<unsigned char>(bytes[offset]);
    const bool has_second = offset + 1U < bytes.size();
    const bool has_third = offset + 2U < bytes.size();
    const auto second = has_second
                            ? static_cast<unsigned char>(bytes[offset + 1U])
                            : 0U;
    const auto third = has_third
                           ? static_cast<unsigned char>(bytes[offset + 2U])
                           : 0U;
    result.push_back(alphabet[first >> 2U]);
    result.push_back(alphabet[((first & 0x03U) << 4U) | (second >> 4U)]);
    result.push_back(has_second
                         ? alphabet[((second & 0x0fU) << 2U) | (third >> 6U)]
                         : '=');
    result.push_back(has_third ? alphabet[third & 0x3fU] : '=');
  }
  return result;
}

[[nodiscard]] deployment::CanonicalValue artifact_payload(
    std::vector<deployment::detail::PreparedActivationArtifact> artifacts) {
  std::ranges::sort(artifacts, {}, [](const auto &artifact) {
    return artifact.content_addressed_store_key.value();
  });
  deployment::CanonicalValue::Array rows;
  rows.reserve(artifacts.size());
  constexpr std::size_t chunk_bytes = 192U * 1024U;
  for (const auto &artifact : artifacts) {
    if (artifact.bytes.empty()) {
      throw std::logic_error("prepared activation contains an empty artifact");
    }
    deployment::CanonicalValue::Array chunks;
    for (std::size_t offset = 0U; offset < artifact.bytes.size();
         offset += chunk_bytes) {
      chunks.emplace_back(base64_encode(std::string_view(artifact.bytes).substr(
          offset, std::min(chunk_bytes, artifact.bytes.size() - offset))));
    }
    rows.emplace_back(deployment::CanonicalValue::Object{
        {"byte_size",
         deployment::CanonicalValue(static_cast<double>(artifact.bytes.size()))},
        {"chunks", deployment::CanonicalValue(std::move(chunks))},
        {"content_addressed_store_key",
         deployment::CanonicalValue(
             artifact.content_addressed_store_key.value())},
        {"content_digest",
         deployment::CanonicalValue(artifact.content_digest.value())},
        {"encoding", deployment::CanonicalValue("base64")},
    });
  }
  return deployment::CanonicalValue(std::move(rows));
}

[[nodiscard]] deployment::CanonicalValue prepare(const Options &options) {
  const auto request =
      deployment::parse_strict_json_model_yaml(read_file(options.input));
  const auto &root = request.root();
  require_exact_fields(root,
                       {"schema_version", "source_document_digest",
                        "scenario_id", "captured_input_digest", "deployment",
                        "model_source_closure", "prepared_model_closure",
                        "program",
                        "program_role_assignment", "typed_parameter_overrides"},
                       "activation preparation request");
  if (text(member(root, "schema_version", "request"), "schema_version") !=
      "manyforge.activation_preparation_request.v1") {
    throw std::invalid_argument("unsupported activation preparation request");
  }
  const auto &source_digest =
      text(member(root, "source_document_digest", "request"),
           "source_document_digest");
  const auto &captured_digest =
      text(member(root, "captured_input_digest", "request"),
           "captured_input_digest");
  const auto &scenario_id =
      text(member(root, "scenario_id", "request"), "scenario_id");
  if (scenario_id.size() > 256U) {
    throw std::invalid_argument("scenario_id exceeds its byte bound");
  }
  if (!deployment::is_sha256_digest(source_digest) ||
      !deployment::is_sha256_digest(captured_digest)) {
    throw std::invalid_argument(
        "preparation digests must be SHA-256 identities");
  }
  const auto &deployment_value = member(root, "deployment", "request");
  const auto &model_source_closure =
      member(root, "model_source_closure", "request");
  const auto &prepared_model_closure =
      member(root, "prepared_model_closure", "request");
  const auto &program_value = member(root, "program", "request");
  const auto &assignment_value =
      member(root, "program_role_assignment", "request");
  const auto &overrides = member(root, "typed_parameter_overrides", "request");
  static_cast<void>(object(deployment_value, "deployment"));
  static_cast<void>(object(model_source_closure, "model_source_closure"));
  static_cast<void>(object(prepared_model_closure, "prepared_model_closure"));
  static_cast<void>(object(program_value, "program"));
  static_cast<void>(object(overrides, "typed_parameter_overrides"));
  const deployment::CanonicalValue captured(deployment::CanonicalValue::Object{
      {"deployment", deployment_value},
      {"model_source_closure", model_source_closure},
      {"program", program_value},
      {"program_role_assignment", assignment_value},
      {"scenario_id", deployment::CanonicalValue(scenario_id)},
      {"source_document_digest", deployment::CanonicalValue(source_digest)},
      {"typed_parameter_overrides", overrides},
  });
  if (deployment::sha256_digest(deployment::canonical_json(captured)) !=
      captured_digest) {
    throw std::invalid_argument("captured_input_digest does not match request");
  }
  auto captured_sources =
      deployment::detail::parse_activation_model_source_closure(
          model_source_closure);
  const auto deployment_intent =
      deployment::detail::parse_activation_deployment_intent(
          deployment_value, std::move(captured_sources));
  const auto component_profiles =
      deployment::detail::resolve_activation_component_profiles(
          options.capability_root, deployment_intent);
  auto prepared_sources =
      deployment::detail::parse_activation_prepared_model_closure(
          prepared_model_closure);
  std::vector<deployment::detail::ActivationPreparedModel> prepared_models;
  try {
    prepared_models = deployment::detail::qualify_activation_prepared_models(
        deployment_intent, component_profiles, std::move(prepared_sources));
  } catch (const std::length_error &error) {
    throw std::runtime_error(
        std::string("prepared-model qualification produced an invalid size: ") +
        error.what());
  }

  const auto materialized = deployment::detail::materialize_prepared_activation(
      options.capability_root, std::move(prepared_models),
      deployment_intent, component_profiles, options.publish_canonical_empty);
  const auto program = deployment::verify_program_binding_contract(
      deployment::canonical_json(program_value), *materialized.bundle);
  const auto mode = assistant_mode(program_value);
  deployment::verify_scenario_program_admission(
      program->normalized_program_root(), materialized.bundle->value(),
      materialized.deployment_compatibility, mode, overrides);
  const PreparedCatalogSource catalog_store(materialized.catalog_artifacts);
  const auto binding = deployment::compile_program_binding(
      *program, parse_assignment(assignment_value), *materialized.bundle,
      materialized.model_map, catalog_store);
  const auto reverified_program = deployment::verify_program_binding_contract(
      program->canonical_program_bytes(), *materialized.bundle);
  const auto reverified_binding = deployment::reload_verified_program_binding(
      binding->canonical_bytes(), *materialized.bundle, materialized.model_map);
  if (reverified_program == nullptr || reverified_binding == nullptr ||
      reverified_program->value().program_id != program->value().program_id ||
      reverified_program->value().program_revision !=
          program->value().program_revision ||
      reverified_binding->program_binding_id() !=
          binding->program_binding_id()) {
    throw std::logic_error(
        "prepared Program or binding failed independent in-memory reload");
  }
  deployment::CanonicalValue::Object identity{
      {"activation_payload",
       deployment::CanonicalValue(deployment::CanonicalValue::Object{
           {"catalog_artifacts",
            artifact_payload(materialized.catalog_artifacts)},
           {"program", program->normalized_program_root()},
           {"program_binding", binding->normalized_root()},
           {"raw_artifacts", artifact_payload(materialized.raw_artifacts)},
           {"retained_bundle", materialized.bundle->normalized_root()},
       })},
      {"assistant_mode",
       deployment::CanonicalValue(std::string(assistant_mode_text(mode)))},
      {"captured_input_digest", deployment::CanonicalValue(captured_digest)},
      {"deployment_artifact_id",
       deployment::CanonicalValue(materialized.deployment_artifact_id.value())},
      {"deployment_revision",
       deployment::CanonicalValue(materialized.deployment_revision.value())},
      {"graph_revision",
       deployment::CanonicalValue(
           materialized.bundle->value().cell_graph.graph_revision.value())},
      {"node_environment_digest",
       deployment::CanonicalValue(
           materialized.bundle->value()
               .catalogs.node_environment.environment_digest.value())},
      {"program_binding_id",
       deployment::CanonicalValue(binding->program_binding_id().value())},
      {"program_id",
       deployment::CanonicalValue(program->value().program_id.value())},
      {"program_revision",
       deployment::CanonicalValue(program->value().program_revision.value())},
      {"retained_bundle_id",
       deployment::CanonicalValue(
           materialized.bundle->retained_bundle_id().value())},
      {"scenario_id", deployment::CanonicalValue(scenario_id)},
      {"schema_version",
       deployment::CanonicalValue("manyforge.prepared_activation_input.v2")},
      {"source_document_digest", deployment::CanonicalValue(source_digest)},
      {"typed_parameter_overrides", overrides},
  };
  const auto prepared_id = deployment::sha256_digest(
      deployment::canonical_json(deployment::CanonicalValue(identity)));
  identity.emplace_back("prepared_activation_id",
                        deployment::CanonicalValue(prepared_id));
  const deployment::CanonicalValue result(std::move(identity));
  auto prepared = deployment::parse_prepared_activation_input(result);
  materialize_activation(prepared, options.activation_root);
  if (options.publish_canonical_empty) {
    if (materialized.canonical_empty_bundle == nullptr) {
      throw std::logic_error(
          "prepared activation omitted its canonical empty predecessor");
    }
    // The ordinary activation store above publishes the candidate only.
    // Publish its separately qualified empty predecessor before admitting it.
    deployment::FilesystemRetainedBundleRepository bundles(
        options.activation_root);
    static_cast<void>(
        bundles.put_if_absent(materialized.canonical_empty_bundle));
    // The in-memory preparer qualified these same build authorities. The
    // package verifier reads their persisted form before admitting the empty
    // predecessor, so publish them in this package before its commit record.
    deployment::FilesystemBuildManifestRepository manifests(
        options.activation_root);
    static_cast<void>(
        deployment::detail::BuildQualifiedRetentionAuthority::qualify_persisted(
            manifests));
    const auto admission_filename =
        deployment::cell_scoped_canonical_empty_admission_filename(
            materialized.canonical_empty_bundle->value().cell_graph.cell_id);
    const auto admission =
        deployment::detail::publish_cell_scoped_canonical_empty_admission(
            options.activation_root, admission_filename,
            *materialized.canonical_empty_bundle);
    if (admission.retained_bundle_id() !=
        materialized.canonical_empty_bundle->retained_bundle_id()) {
      throw std::logic_error(
          "prepared canonical empty predecessor did not reload exactly");
    }
  }
  return prepared.normalized_root();
}

} // namespace

int main(int argc, char **argv) {
  try {
    auto options = parse_options(argc, argv);
    options.input = std::filesystem::canonical(options.input);
    options.capability_root =
        std::filesystem::canonical(options.capability_root);
    options.activation_root =
        std::filesystem::canonical(options.activation_root);
    std::cout << deployment::canonical_json(prepare(options)) << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "manyforge_prepare_activation: " << error.what() << '\n';
    return 2;
  }
}
