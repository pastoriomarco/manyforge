#include <manyforge_deployment/prepared_activation_input.hpp>

#include <manyforge_deployment/content_identity.hpp>

#include "verified_retained_bundle_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

PreparedActivationInput::PreparedActivationInput(
    core::ContentDigest prepared_activation_id,
    core::ContentDigest source_document_digest,
    std::string scenario_id,
    core::ContentDigest captured_input_digest,
    core::DeploymentArtifactId deployment_artifact_id,
    core::DeploymentRevision deployment_revision,
    core::RetainedBundleId retained_bundle_id,
    core::GraphRevision graph_revision, core::ProgramId program_id,
    core::ProgramRevision program_revision,
    core::ProgramBindingId program_binding_id,
    core::ContentDigest node_environment_digest, std::string assistant_mode,
    CanonicalValue typed_parameter_overrides, CanonicalValue normalized_root,
    VerifiedPreparedActivationClosure closure)
    : prepared_activation_id_(std::move(prepared_activation_id)),
      source_document_digest_(std::move(source_document_digest)),
      scenario_id_(std::move(scenario_id)),
      captured_input_digest_(std::move(captured_input_digest)),
      deployment_artifact_id_(std::move(deployment_artifact_id)),
      deployment_revision_(std::move(deployment_revision)),
      retained_bundle_id_(std::move(retained_bundle_id)),
      graph_revision_(std::move(graph_revision)), program_id_(std::move(program_id)),
      program_revision_(std::move(program_revision)),
      program_binding_id_(std::move(program_binding_id)),
      node_environment_digest_(std::move(node_environment_digest)),
      assistant_mode_(std::move(assistant_mode)),
      typed_parameter_overrides_(std::move(typed_parameter_overrides)),
      normalized_root_(std::move(normalized_root)), closure_(std::move(closure)) {}

PreparedActivationInput
parse_prepared_activation_input(const CanonicalValue &value) {
  require_exact_fields(
      value,
      {"activation_payload", "assistant_mode", "captured_input_digest",
       "deployment_artifact_id", "deployment_revision", "graph_revision",
       "node_environment_digest", "prepared_activation_id",
       "program_binding_id", "program_id", "program_revision",
       "retained_bundle_id", "scenario_id", "schema_version",
       "source_document_digest",
       "typed_parameter_overrides"},
      "prepared activation input");
  if (text(member(value, "schema_version"), "schema_version") !=
      kPreparedActivationInputSchemaVersion) {
    fail("prepared activation input has an unsupported schema_version");
  }
  const auto &prepared_id =
      text(member(value, "prepared_activation_id"), "prepared_activation_id");
  const auto &source_digest =
      text(member(value, "source_document_digest"), "source_document_digest");
  const auto &scenario_id = text(member(value, "scenario_id"), "scenario_id");
  if (scenario_id.size() > 256U) {
    fail("prepared activation input scenario_id exceeds its bound");
  }
  const auto &captured_digest =
      text(member(value, "captured_input_digest"), "captured_input_digest");
  const auto &deployment_revision =
      text(member(value, "deployment_revision"), "deployment_revision");
  const auto &retained_bundle_id =
      text(member(value, "retained_bundle_id"), "retained_bundle_id");
  const auto &graph_revision =
      text(member(value, "graph_revision"), "graph_revision");
  const auto &program_revision =
      text(member(value, "program_revision"), "program_revision");
  const auto &program_binding_id =
      text(member(value, "program_binding_id"), "program_binding_id");
  const auto &node_environment_digest =
      text(member(value, "node_environment_digest"), "node_environment_digest");
  require_digest(prepared_id, "prepared_activation_id");
  require_digest(source_digest, "source_document_digest");
  require_digest(captured_digest, "captured_input_digest");
  require_digest(deployment_revision, "deployment_revision");
  require_digest(retained_bundle_id, "retained_bundle_id");
  require_digest(graph_revision, "graph_revision");
  require_digest(program_revision, "program_revision");
  require_digest(program_binding_id, "program_binding_id");
  require_digest(node_environment_digest, "node_environment_digest");
  const auto &assistant_mode = text(member(value, "assistant_mode"), "assistant_mode");
  if (assistant_mode != "required" && assistant_mode != "optional" &&
      assistant_mode != "forbidden") {
    fail("prepared activation input assistant_mode is unsupported");
  }
  const auto &overrides = member(value, "typed_parameter_overrides");
  static_cast<void>(object(overrides, "typed_parameter_overrides"));

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
                        "raw_artifacts", "retained_bundle"},
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
      bundle->value().compiled_products.symbol_role_maps.size() != 1U) {
    fail("prepared activation retained bundle differs from its summary");
  }
  std::size_t artifact_bytes_total = 0U;
  auto raw_artifacts = parse_artifacts(member(payload, "raw_artifacts"),
                                       "activation_payload.raw_artifacts",
                                       artifact_bytes_total);
  auto catalog_artifacts = parse_artifacts(
      member(payload, "catalog_artifacts"),
      "activation_payload.catalog_artifacts", artifact_bytes_total);
  const auto &map_product =
      bundle->value().compiled_products.symbol_role_maps.front();
  if (map_product.artifact_refs.size() != 1U) {
    fail("prepared activation retained model map is incoherent");
  }
  const RetainedModelMapQuery map_query{bundle->retained_bundle_id(),
                                        map_product.product_id};
  auto model_map = detail::RetainedModelMapVerificationAuthority::verify(
      *bundle, map_query,
      artifact_bytes(raw_artifacts, map_product.artifact_refs.front()));
  if (!model_map.has_value()) {
    fail("prepared activation retained model map is unverifiable");
  }
  auto program = verify_program_binding_contract(
      canonical_json(member(payload, "program")), *bundle);
  auto binding = reload_verified_program_binding(
      canonical_json(member(payload, "program_binding")), *bundle, *model_map);
  if (program == nullptr || binding == nullptr ||
      program->value().program_id.value() !=
          text(member(value, "program_id"), "program_id") ||
      program->value().program_revision.value() != program_revision ||
      binding->program_binding_id().value() != program_binding_id ||
      binding->value().program_id != program->value().program_id ||
      binding->value().program_revision != program->value().program_revision) {
    fail("prepared activation Program or binding differs from its summary");
  }
  const bool assistant_available = bundle->value().assistant.has_value();
  if ((assistant_mode == "required" && !assistant_available) ||
      (assistant_mode == "forbidden" && assistant_available)) {
    fail("prepared activation assistant mode differs from retained contents");
  }

  VerifiedPreparedActivationClosure closure{
      bundle, std::move(*model_map), std::move(program), std::move(binding),
      std::move(raw_artifacts), std::move(catalog_artifacts)};
  return PreparedActivationInput(
      core::ContentDigest(prepared_id), core::ContentDigest(source_digest),
      scenario_id,
      core::ContentDigest(captured_digest),
      core::DeploymentArtifactId(text(member(value, "deployment_artifact_id"),
                                      "deployment_artifact_id")),
      core::DeploymentRevision(deployment_revision),
      core::RetainedBundleId(retained_bundle_id),
      core::GraphRevision(graph_revision),
      core::ProgramId(text(member(value, "program_id"), "program_id")),
      core::ProgramRevision(program_revision),
      core::ProgramBindingId(program_binding_id),
      core::ContentDigest(node_environment_digest), assistant_mode, overrides,
      value, std::move(closure));
}

} // namespace manyforge::deployment
