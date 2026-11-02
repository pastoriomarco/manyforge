#include <manyforge_deployment/prepared_activation_input.hpp>

#include <manyforge_deployment/content_identity.hpp>

#include "verified_retained_bundle_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <variant>

namespace manyforge::deployment {
namespace {

inline constexpr std::size_t kMaxPreparedArtifactCount = 4096U;
inline constexpr std::size_t kMaxPreparedArtifactBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kMaxPreparedClosureBytes = 96U * 1024U * 1024U;

[[noreturn]] void fail(const std::string &message) {
  throw PreparedActivationInputError(message);
}

[[nodiscard]] const CanonicalValue::Object &
object(const CanonicalValue &value, const std::string_view path) {
  const auto *result = std::get_if<CanonicalValue::Object>(&value.storage());
  if (result == nullptr) fail(std::string(path) + " must be an object");
  return *result;
}

[[nodiscard]] const CanonicalValue::Array &
array(const CanonicalValue &value, const std::string_view path) {
  const auto *result = std::get_if<CanonicalValue::Array>(&value.storage());
  if (result == nullptr) fail(std::string(path) + " must be an array");
  return *result;
}

[[nodiscard]] const CanonicalValue &member(const CanonicalValue &value,
                                           const std::string_view name) {
  const auto *result = value.find_object_member(name);
  if (result == nullptr) {
    fail("prepared activation input is missing " + std::string(name));
  }
  return *result;
}

[[nodiscard]] const std::string &text(const CanonicalValue &value,
                                      const std::string_view name) {
  const auto *result = std::get_if<std::string>(&value.storage());
  if (result == nullptr || result->empty()) {
    fail("prepared activation input " + std::string(name) +
         " must be non-empty text");
  }
  return *result;
}

void require_exact_fields(const CanonicalValue &value,
                          const std::set<std::string, std::less<>> &expected,
                          const std::string_view label) {
  std::set<std::string, std::less<>> actual;
  for (const auto &[name, ignored] : object(value, label)) {
    static_cast<void>(ignored);
    actual.insert(name);
  }
  if (actual == expected) return;
  for (const auto &name : expected) {
    if (!actual.contains(name)) fail(std::string(label) + " is missing " + name);
  }
  for (const auto &name : actual) {
    if (!expected.contains(name)) {
      fail(std::string(label) + " has unexpected field " + name);
    }
  }
  fail(std::string(label) + " fields are inconsistent");
}

void require_digest(const std::string &value, const std::string_view name) {
  if (!is_sha256_digest(value)) {
    fail("prepared activation input " + std::string(name) +
         " must be a SHA-256 identity");
  }
}

[[nodiscard]] std::size_t size_value(const CanonicalValue &value,
                                     const std::string_view label) {
  const auto *number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number) || *number < 0.0 ||
      std::floor(*number) != *number ||
      *number > static_cast<double>(kMaxPreparedArtifactBytes)) {
    fail(std::string(label) + " is outside the prepared-artifact byte bound");
  }
  return static_cast<std::size_t>(*number);
}

[[nodiscard]] int base64_value(const char value) noexcept {
  if (value >= 'A' && value <= 'Z') return value - 'A';
  if (value >= 'a' && value <= 'z') return value - 'a' + 26;
  if (value >= '0' && value <= '9') return value - '0' + 52;
  if (value == '+') return 62;
  if (value == '/') return 63;
  return -1;
}

void decode_base64_chunk(const std::string_view encoded, std::string &output,
                         const std::string_view label) {
  if (encoded.empty() || encoded.size() % 4U != 0U ||
      encoded.size() > 512U * 1024U) {
    fail(std::string(label) + " is not one bounded base64 chunk");
  }
  for (std::size_t offset = 0U; offset < encoded.size(); offset += 4U) {
    const bool third_padding = encoded[offset + 2U] == '=';
    const bool fourth_padding = encoded[offset + 3U] == '=';
    if ((third_padding && !fourth_padding) ||
        ((third_padding || fourth_padding) && offset + 4U != encoded.size())) {
      fail(std::string(label) + " has invalid base64 padding");
    }
    const int first = base64_value(encoded[offset]);
    const int second = base64_value(encoded[offset + 1U]);
    const int third = third_padding ? 0 : base64_value(encoded[offset + 2U]);
    const int fourth = fourth_padding ? 0 : base64_value(encoded[offset + 3U]);
    if (first < 0 || second < 0 || third < 0 || fourth < 0 ||
        (third_padding && (second & 0x0f) != 0) ||
        (fourth_padding && !third_padding && (third & 0x03) != 0)) {
      fail(std::string(label) + " is not canonical base64");
    }
    output.push_back(static_cast<char>((first << 2) | (second >> 4)));
    if (!third_padding) {
      output.push_back(static_cast<char>(((second & 0x0f) << 4) | (third >> 2)));
    }
    if (!fourth_padding) {
      output.push_back(static_cast<char>(((third & 0x03) << 6) | fourth));
    }
  }
}

[[nodiscard]] std::vector<PreparedActivationArtifact>
parse_artifacts(const CanonicalValue &value, const std::string_view label,
                std::size_t &total_bytes) {
  const auto &rows = array(value, label);
  if (rows.size() > kMaxPreparedArtifactCount) {
    fail(std::string(label) + " exceeds the prepared-artifact count bound");
  }
  std::vector<PreparedActivationArtifact> result;
  result.reserve(rows.size());
  std::string previous_key;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const auto row_label = std::string(label) + "[" + std::to_string(index) + "]";
    require_exact_fields(rows[index],
                         {"byte_size", "chunks", "content_addressed_store_key",
                          "content_digest", "encoding"},
                         row_label);
    const auto &key = text(member(rows[index], "content_addressed_store_key"),
                           "content_addressed_store_key");
    const auto &digest = text(member(rows[index], "content_digest"),
                              "content_digest");
    require_digest(digest, "content_digest");
    const bool valid_key = key.size() <= 1024U && key.front() != '/' &&
                           key.find('\\') == std::string::npos &&
                           key.find("../") == std::string::npos &&
                           key.find("/..") == std::string::npos &&
                           std::ranges::all_of(key, [](const unsigned char value) {
                             return (value >= 'a' && value <= 'z') ||
                                    (value >= 'A' && value <= 'Z') ||
                                    (value >= '0' && value <= '9') ||
                                    value == '/' || value == '.' || value == '_' ||
                                    value == '-' || value == ':';
                           });
    if (!valid_key || (!previous_key.empty() && key <= previous_key)) {
      fail(row_label + " has a noncanonical or duplicate store key");
    }
    previous_key = key;
    if (text(member(rows[index], "encoding"), "encoding") != "base64") {
      fail(row_label + " has an unsupported encoding");
    }
    const auto declared_size =
        size_value(member(rows[index], "byte_size"), row_label + ".byte_size");
    std::string bytes;
    bytes.reserve(declared_size);
    const auto &chunks = array(member(rows[index], "chunks"), row_label + ".chunks");
    if (chunks.empty() || chunks.size() > 128U) {
      fail(row_label + " has an invalid base64 chunk count");
    }
    for (std::size_t chunk = 0U; chunk < chunks.size(); ++chunk) {
      decode_base64_chunk(text(chunks[chunk], "artifact chunk"), bytes,
                          row_label + ".chunks[" + std::to_string(chunk) + "]");
      if (bytes.size() > declared_size) {
        fail(row_label + " expands beyond its declared byte size");
      }
    }
    if (bytes.size() != declared_size || sha256_digest(bytes) != digest) {
      fail(row_label + " differs from its declared size or digest");
    }
    if (total_bytes > kMaxPreparedClosureBytes - bytes.size()) {
      fail("prepared activation artifact closure exceeds its byte bound");
    }
    total_bytes += bytes.size();
    result.push_back({core::ContentAddressedStoreKey(key),
                      core::ContentDigest(digest), std::move(bytes)});
  }
  return result;
}

[[nodiscard]] const std::string &artifact_bytes(
    const std::vector<PreparedActivationArtifact> &artifacts,
    const core::RetainedArtifactRefDTO &reference) {
  const auto found = std::find_if(artifacts.begin(), artifacts.end(),
                                  [&](const auto &artifact) {
    return artifact.content_addressed_store_key ==
               reference.content_addressed_store_key &&
           artifact.content_digest == reference.content_digest &&
           artifact.bytes.size() == reference.byte_size;
  });
  if (found == artifacts.end()) {
    fail("prepared activation model-map artifact is absent");
  }
  return found->bytes;
}

} // namespace

core::Result<PreparedActivationProgramOutcomeDTO>
parse_prepared_activation_program_outcome(const CanonicalValue &prepared) {
  using Reply = core::Result<PreparedActivationProgramOutcomeDTO>;
  const auto missing = [](std::string_view field) {
    return core::ErrorDTO{"headless_source_invalid",
                          "headless source is missing field " +
                              std::string(field),
                          {}};
  };
  const auto string_value =
      [&](const CanonicalValue &object,
          std::string_view field) -> core::Result<std::string> {
    const auto *value = object.find_object_member(field);
    if (!value)
      return core::Result<std::string>::Err(missing(field));
    if (!value->is_string() || value->as_string().empty())
      return core::Result<std::string>::Err({"headless_source_invalid",
                                             "headless source field " +
                                                 std::string(field) +
                                                 " must be a non-empty string",
                                             {}});
    return core::Result<std::string>::Ok(value->as_string());
  };
  const auto invalid = [](std::string message) {
    return Reply::Err(
        {"program_applied_selection_invalid", std::move(message), {}});
  };
  const auto *outcome = prepared.find_object_member("program_outcome");
  if (!outcome)
    return Reply::Err(missing("program_outcome"));
  const auto kind = string_value(*outcome, "kind");
  if (!kind.ok())
    return Reply::Err(kind.error());
  if (kind.value() == "no_program") {
    if (!outcome->is_object() || outcome->as_object().size() != 1U)
      return invalid("inconsistent no_program outcome");
    for (const auto field : {"program_id", "program_revision"}) {
      const auto *value = prepared.find_object_member(field);
      if (!value)
        return Reply::Err(missing(field));
      if (!value->is_null())
        return invalid("inconsistent no_program outcome");
    }
    return Reply::Ok({});
  }
  if (kind.value() == "bound") {
    const auto binding = string_value(*outcome, "program_binding_id");
    if (!binding.ok())
      return Reply::Err(binding.error());
    return Reply::Ok({core::ProgramBindingId(binding.value()), std::nullopt});
  }
  if (kind.value() != "non_executable")
    return invalid("active Program outcome kind is unsupported");
  const auto *encoded = outcome->find_object_member("outcome");
  if (!encoded)
    return Reply::Err(missing("outcome"));
  try {
    auto decoded = DecodeProgramNonExecutableOutcome(canonical_json(*encoded));
    const std::pair<std::string_view, std::string_view> fields[] = {
        {"program_id", decoded.program_id.value()},
        {"program_revision", decoded.program_revision.value()},
        {"retained_bundle_id", decoded.retained_bundle_id.value()},
        {"graph_revision", decoded.graph_revision.value()}};
    for (const auto &[field, expected] : fields) {
      const auto actual = string_value(prepared, field);
      if (!actual.ok())
        return Reply::Err(actual.error());
      if (actual.value() != expected)
        return invalid("active Program outcome differs from its preparation");
    }
    return Reply::Ok({std::nullopt, std::move(decoded)});
  } catch (const ProgramBindingOutcomeError &error) {
    return invalid(std::string("active Program outcome is invalid: ") +
                   error.what());
  }
}

PreparedScenarioOrigin
parse_prepared_activation_origin(const CanonicalValue &value) {
  if (text(member(value, "kind"), "origin.kind") != "scenario")
    fail("prepared activation origin kind is unsupported");
  require_exact_fields(
      value,
      {"kind", "scenario_id", "source_document_digest", "project_path"},
      "origin");
  const auto &scenario = text(member(value, "scenario_id"), "origin.scenario_id");
  if (scenario.size() > 256U) fail("prepared activation origin scenario_id exceeds its bound");
  const auto &source = text(member(value, "source_document_digest"), "origin.source_document_digest");
  require_digest(source, "origin.source_document_digest");
  const auto &project =
      text(member(value, "project_path"), "origin.project_path");
  const std::filesystem::path path(project);
  if (project.size() > 4096U || project.find('\0') != std::string::npos ||
      !path.is_absolute() ||
      path.lexically_normal().generic_string() != project ||
      (project.size() > 1U && project.back() == '/'))
    fail("prepared activation origin project_path must be a bounded "
         "normalized absolute folder");
  return PreparedScenarioOrigin{scenario, core::ContentDigest(source),
                                project};
}

CanonicalValue prepared_activation_origin_value(const PreparedScenarioOrigin &origin) {
  return CanonicalValue(CanonicalValue::Object{
      {"kind", CanonicalValue("scenario")},
      {"scenario_id", CanonicalValue(origin.scenario_id)},
      {"source_document_digest",
       CanonicalValue(origin.source_document_digest.value())},
      {"project_path", CanonicalValue(origin.project_path)}});
}

PreparedActivationInput::PreparedActivationInput(
    core::ContentDigest prepared_activation_id,
    PreparedScenarioOrigin origin,
    core::ContentDigest captured_input_digest,
    core::DeploymentArtifactId deployment_artifact_id,
    core::DeploymentRevision deployment_revision,
    core::RetainedBundleId retained_bundle_id,
    core::GraphRevision graph_revision, std::optional<core::ProgramId> program_id,
    std::optional<core::ProgramRevision> program_revision,
    std::optional<core::ProgramBindingId> program_binding_id,
    std::optional<ProgramNonExecutableOutcomeDTO> program_outcome,
    core::ProgramRoleAssignmentDTO program_role_assignment,
    core::ContentDigest node_environment_digest, std::string assistant_mode,
    CanonicalValue normalized_root,
    VerifiedPreparedActivationClosure closure)
    : prepared_activation_id_(std::move(prepared_activation_id)),
      origin_(std::move(origin)),
      captured_input_digest_(std::move(captured_input_digest)),
      deployment_artifact_id_(std::move(deployment_artifact_id)),
      deployment_revision_(std::move(deployment_revision)),
      retained_bundle_id_(std::move(retained_bundle_id)),
      graph_revision_(std::move(graph_revision)), program_id_(std::move(program_id)),
      program_revision_(std::move(program_revision)),
      program_binding_id_(std::move(program_binding_id)),
      program_outcome_(std::move(program_outcome)),
      program_role_assignment_(std::move(program_role_assignment)),
      node_environment_digest_(std::move(node_environment_digest)),
      assistant_mode_(std::move(assistant_mode)),
      normalized_root_(std::move(normalized_root)), closure_(std::move(closure)) {}

PreparedActivationInput
parse_prepared_activation_input(const CanonicalValue &value) {
  // Development cutover: only the current version opens. v3/v4 carried the
  // removed launch-time typed_parameter_overrides and are refused, never
  // reinterpreted; Program parameters always take their Program defaults.
  if (const auto *schema = value.find_object_member("schema_version");
      schema != nullptr &&
      std::get_if<std::string>(&schema->storage()) != nullptr &&
      std::get<std::string>(schema->storage()) !=
          kPreparedActivationInputSchemaVersion) {
    fail("prepared activation input schema_version " +
         std::get<std::string>(schema->storage()) +
         " is unsupported; only " +
         std::string(kPreparedActivationInputSchemaVersion) +
         " opens (launch-time Program parameter overrides were removed); "
         "prepare the Scenario again from a fresh stopped workbench");
  }
  require_exact_fields(
      value,
      {"activation_payload", "assistant_mode", "captured_input_digest",
       "deployment_artifact_id", "deployment_revision", "graph_revision",
       "node_environment_digest", "prepared_activation_id",
       "program_id", "program_outcome", "program_revision",
       "retained_bundle_id", "origin", "schema_version"},
      "prepared activation input");
  const auto &schema = text(member(value, "schema_version"), "schema_version");
  if (schema != kPreparedActivationInputSchemaVersion) {
    fail("prepared activation input has an unsupported schema_version");
  }
  const auto &prepared_id =
      text(member(value, "prepared_activation_id"), "prepared_activation_id");
  auto origin = parse_prepared_activation_origin(member(value, "origin"));
  const auto &captured_digest =
      text(member(value, "captured_input_digest"), "captured_input_digest");
  const auto &deployment_revision =
      text(member(value, "deployment_revision"), "deployment_revision");
  const auto &retained_bundle_id =
      text(member(value, "retained_bundle_id"), "retained_bundle_id");
  const auto &graph_revision =
      text(member(value, "graph_revision"), "graph_revision");
  // Exactly one outcome: bound, named non-executable, or explicitly absent.
  const auto &outcome_value = member(value, "program_outcome");
  const auto &outcome_kind =
      text(member(outcome_value, "kind"), "program_outcome.kind");
  std::optional<std::string> program_binding_id;
  std::optional<ProgramNonExecutableOutcomeDTO> program_outcome;
  const bool no_program = outcome_kind == "no_program";
  if (no_program) {
    require_exact_fields(outcome_value, {"kind"}, "program_outcome");
    if (!member(value, "program_id").is_null() ||
        !member(value, "program_revision").is_null()) {
      fail("no_program must not carry Program identities");
    }
  } else if (outcome_kind == "bound") {
    require_exact_fields(outcome_value, {"kind", "program_binding_id"},
                         "program_outcome");
    program_binding_id = text(member(outcome_value, "program_binding_id"),
                              "program_outcome.program_binding_id");
  } else if (outcome_kind == "non_executable") {
    require_exact_fields(outcome_value, {"kind", "outcome"}, "program_outcome");
    try {
      program_outcome = DecodeProgramNonExecutableOutcome(
          canonical_json(member(outcome_value, "outcome")));
    } catch (const ProgramBindingOutcomeError &error) {
      fail(std::string("prepared activation Program outcome is invalid: ") +
           error.what());
    }
  } else {
    fail("prepared activation Program outcome kind is unsupported");
  }
  const auto &node_environment_digest =
      text(member(value, "node_environment_digest"), "node_environment_digest");
  require_digest(prepared_id, "prepared_activation_id");
  require_digest(captured_digest, "captured_input_digest");
  require_digest(deployment_revision, "deployment_revision");
  require_digest(retained_bundle_id, "retained_bundle_id");
  require_digest(graph_revision, "graph_revision");
  std::optional<core::ProgramId> program_id;
  std::optional<core::ProgramRevision> program_revision;
  if (!no_program) {
    program_id = core::ProgramId(text(member(value, "program_id"), "program_id"));
    const auto &revision = text(member(value, "program_revision"), "program_revision");
    require_digest(revision, "program_revision");
    program_revision = core::ProgramRevision(revision);
  }
  if (program_binding_id.has_value()) {
    require_digest(*program_binding_id, "program_binding_id");
  }
  require_digest(node_environment_digest, "node_environment_digest");
  const auto &assistant_mode = text(member(value, "assistant_mode"), "assistant_mode");
  if (assistant_mode != "required" && assistant_mode != "optional" &&
      assistant_mode != "forbidden") {
    fail("prepared activation input assistant_mode is unsupported");
  }

  CanonicalValue::Object identity;
  for (const auto &[name, field] : object(value, "prepared activation input")) {
    if (name != "prepared_activation_id") identity.emplace_back(name, field);
  }
  if (sha256_digest(canonical_json(CanonicalValue(std::move(identity)))) !=
      prepared_id) {
    fail("prepared activation input has a false content identity");
  }

  const auto &payload = member(value, "activation_payload");
  require_exact_fields(payload,
                       {"catalog_artifacts", "program", "program_binding",
                        "program_role_assignment", "raw_artifacts",
                        "retained_bundle"},
                       "activation_payload");
  auto bundle = detail::RetainedBundleReloadAuthority::reload(
      canonical_json(member(payload, "retained_bundle")));
  if (bundle == nullptr || bundle->retained_bundle_id().value() != retained_bundle_id ||
      bundle->value().deployment_artifact_id.value() !=
          text(member(value, "deployment_artifact_id"), "deployment_artifact_id") ||
      bundle->value().deployment_revision.value() != deployment_revision ||
      bundle->value().cell_graph.graph_revision.value() != graph_revision ||
      bundle->value().catalogs.node_environment.environment_digest.value() !=
          node_environment_digest ||
      bundle->value().compiled_products.symbol_role_maps.size() > 1U) {
    fail("prepared activation retained bundle differs from its summary");
  }
  std::size_t artifact_bytes_total = 0U;
  auto raw_artifacts = parse_artifacts(member(payload, "raw_artifacts"),
                                       "activation_payload.raw_artifacts",
                                       artifact_bytes_total);
  auto catalog_artifacts = parse_artifacts(
      member(payload, "catalog_artifacts"),
      "activation_payload.catalog_artifacts", artifact_bytes_total);
  std::optional<VerifiedRetainedModelMap> model_map;
  if (!bundle->value().compiled_products.symbol_role_maps.empty()) {
    const auto &map_product =
        bundle->value().compiled_products.symbol_role_maps.front();
    if (map_product.artifact_refs.size() != 1U) {
      fail("prepared activation retained model map is incoherent");
    }
    const RetainedModelMapQuery map_query{bundle->retained_bundle_id(),
                                          map_product.product_id};
    model_map = detail::RetainedModelMapVerificationAuthority::verify(
        *bundle, map_query,
        artifact_bytes(raw_artifacts, map_product.artifact_refs.front()));
    if (!model_map.has_value()) {
      fail("prepared activation retained model map is unverifiable");
    }
  } else if (!bundle->value().cell_graph.controllable_components.empty() ||
             !bundle->value().cell_graph.model_instances.empty() ||
             program_binding_id.has_value() || (!no_program && !program_outcome.has_value()) ||
             (!no_program && !raw_artifacts.empty())) {
    fail("prepared activation may omit model data only for an empty candidate "
         "with no executable Program");
  }
  std::shared_ptr<const ProgramBindingContract> program;
  if (no_program) {
    if (!member(payload, "program").is_null() || assistant_mode != "optional") {
      fail("no_program must have no Program and optional assistant mode");
    }
  } else {
    program = verify_program_binding_contract(
        canonical_json(member(payload, "program")), *bundle);
    if (program == nullptr || program->value().program_id != *program_id ||
        program->value().program_revision != *program_revision) {
      fail("prepared activation Program differs from its summary");
    }
  }
  const auto &binding_value = member(payload, "program_binding");
  std::shared_ptr<const ProgramBinding> binding;
  if (program_binding_id.has_value()) {
    if (!model_map.has_value()) {
      fail("prepared activation binding has no retained model map");
    }
    binding = reload_verified_program_binding(canonical_json(binding_value),
                                              *bundle, *model_map);
    if (binding == nullptr ||
        binding->program_binding_id().value() != *program_binding_id ||
        binding->value().program_id != program->value().program_id ||
        binding->value().program_revision != program->value().program_revision) {
      fail("prepared activation Program binding differs from its summary");
    }
  } else if (no_program) {
    if (!binding_value.is_null()) {
      fail("no_program must not carry a Program binding");
    }
  } else if (!binding_value.is_null() ||
             program_outcome->program_id != program->value().program_id ||
             program_outcome->program_revision !=
                 program->value().program_revision ||
             program_outcome->retained_bundle_id !=
                 bundle->retained_bundle_id() ||
             program_outcome->graph_revision !=
                 bundle->value().cell_graph.graph_revision) {
    // A non-executable outcome is bound to this exact Program and candidate
    // and never carries (or substitutes) a binding.
    fail("prepared activation non-executable Program outcome differs from "
         "its summary");
  }
  core::ProgramRoleAssignmentDTO role_assignment;
  try {
    role_assignment =
        parse_program_role_assignment(member(payload, "program_role_assignment"));
  } catch (const ProgramBindingOutcomeError &error) {
    fail(std::string("prepared activation role assignment is invalid: ") +
         error.what());
  }
  if (no_program && (!role_assignment.component_roles.empty() ||
                     !role_assignment.target_roles.empty())) {
    fail("no_program must not carry role assignments");
  }
  const bool assistant_available = bundle->value().assistant.has_value();
  if ((assistant_mode == "required" && !assistant_available) ||
      (assistant_mode == "forbidden" && assistant_available)) {
    fail("prepared activation assistant mode differs from retained contents");
  }

  VerifiedPreparedActivationClosure closure{
      bundle, std::move(model_map), std::move(program), std::move(binding),
      std::move(raw_artifacts), std::move(catalog_artifacts)};
  return PreparedActivationInput(
      core::ContentDigest(prepared_id), std::move(origin),
      core::ContentDigest(captured_digest),
      core::DeploymentArtifactId(text(member(value, "deployment_artifact_id"),
                                      "deployment_artifact_id")),
      core::DeploymentRevision(deployment_revision),
      core::RetainedBundleId(retained_bundle_id),
      core::GraphRevision(graph_revision),
      std::move(program_id), std::move(program_revision),
      program_binding_id.has_value()
          ? std::optional(core::ProgramBindingId(*program_binding_id))
          : std::nullopt,
      std::move(program_outcome), std::move(role_assignment),
      core::ContentDigest(node_environment_digest), assistant_mode, value,
      std::move(closure));
}

} // namespace manyforge::deployment
