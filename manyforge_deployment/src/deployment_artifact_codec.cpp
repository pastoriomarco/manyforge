#include "manyforge_deployment/deployment_artifact_codec.hpp"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "content_identity_internal.hpp"
#include "closed_schema_internal.hpp"
#include "compiler_semantic_limits_internal.hpp"
#include "deployment_catalog_codec_internal.hpp"
#include "manyforge_deployment/content_identity.hpp"

namespace manyforge::deployment {
namespace {

constexpr std::string_view kDeploymentApiVersion =
    "manyforge.deployment.v1";
constexpr std::string_view kPlaceholderDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
const detail::ContentIdentitySpec kDeploymentIdentitySpec{
    "deployment-revision",
    std::string(kDeploymentApiVersion),
    "deployment_revision",
    detail::ContentIdentityRootShape::kDocument,
};

[[noreturn]] void fail(std::string_view path, std::string_view reason) {
  throw DeploymentArtifactCodecError(std::string(path) + ": " +
                                     std::string(reason));
}

[[nodiscard]] const CanonicalValue::Object& require_object(
    const CanonicalValue& value, std::string_view path) {
  const auto* object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail(path, "expected object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue::Array& require_array(const CanonicalValue& value,
                                                         std::string_view path) {
  const auto* array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail(path, "expected array");
  }
  return *array;
}

[[nodiscard]] const std::string& require_string(
    const CanonicalValue& value, std::string_view path) {
  const auto* text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail(path, "expected string");
  }
  return *text;
}

[[nodiscard]] const CanonicalValue* find_member(
    const CanonicalValue::Object& object, std::string_view name) noexcept {
  const auto iterator = std::find_if(
      object.begin(), object.end(),
      [name](const auto& member) { return member.first == name; });
  return iterator == object.end() ? nullptr : &iterator->second;
}

[[nodiscard]] const CanonicalValue& require_member(
    const CanonicalValue::Object& object, std::string_view name,
    std::string_view path) {
  const CanonicalValue* value = find_member(object, name);
  if (value == nullptr) {
    fail(path, "missing required field '" + std::string(name) + "'");
  }
  return *value;
}

[[nodiscard]] bool contains(
    std::initializer_list<std::string_view> values,
    std::string_view candidate) {
  return std::find(values.begin(), values.end(), candidate) != values.end();
}

void require_closed_root(const CanonicalValue::Object& root) {
  const std::initializer_list<std::string_view> kRequired{
      "api_version",
      "deployment_artifact_id",
      "deployment_revision",
      "cell_id",
      "name",
      "catalogs",
      "graph",
      "desired_control_configurations",
      "capability_provider_bindings",
  };
  const std::initializer_list<std::string_view> kOptional{
      "description",      "site_overlay_ref", "policies",
      "external_resources", "assistant",        "compatibility",
  };

  for (const auto& [name, unused] : root) {
    (void)unused;
    if (!contains(kRequired, name) && !contains(kOptional, name)) {
      fail("$", "unknown field '" + name + "'");
    }
  }
  for (const std::string_view name : kRequired) {
    (void)require_member(root, name, "$");
  }
}

void require_exact_graph_fields(const CanonicalValue::Object& graph) {
  const std::initializer_list<std::string_view> kGraphFields{
      "runtime_units",
      "model_instances",
      "controllable_components",
      "control_domains",
      "control_bindings",
      "component_attachments",
  };
  for (const auto& [name, unused] : graph) {
    (void)unused;
    if (!contains(kGraphFields, name)) {
      fail("$.graph", "unknown field '" + name + "'");
    }
  }
  for (const std::string_view name : kGraphFields) {
    (void)require_member(graph, name, "$.graph");
  }
}

[[nodiscard]] constexpr bool is_ascii_alphanumeric(
    unsigned char byte) noexcept {
  return (byte >= 'a' && byte <= 'z') ||
         (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9');
}

[[nodiscard]] bool is_portable_token(std::string_view value) noexcept {
  if (value.empty() ||
      value.size() > detail::kMaximumPortableIdentityBytes ||
      !is_ascii_alphanumeric(static_cast<unsigned char>(value.front()))) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](unsigned char byte) {
    return is_ascii_alphanumeric(byte) || byte == '.' || byte == '_' ||
           byte == '-';
  });
}

template <typename StrongType>
[[nodiscard]] StrongType parse_portable_id(const CanonicalValue& value,
                                           std::string_view path) {
  const std::string& text = require_string(value, path);
  if (!is_portable_token(text)) {
    fail(path,
         "expected a 1..128 byte portable token beginning with an ASCII "
         "alphanumeric and containing only alphanumerics, '.', '_', or '-'");
  }
  return StrongType(text);
}

template <typename StrongType>
[[nodiscard]] StrongType parse_digest(const CanonicalValue& value,
                                      std::string_view path) {
  const std::string& text = require_string(value, path);
  if (!is_sha256_digest(text)) {
    fail(path, "expected lowercase sha256:<64 hex> digest");
  }
  return StrongType(text);
}

void validate_compatibility_set(const CanonicalValue& value, std::string_view path) {
  const auto& items = require_array(value, path);
  if (items.size() > detail::kMaximumCompilerContractRows) {
    fail(path, "exceeds fixed compatibility metadata bound");
  }
  std::vector<std::string> parsed;
  parsed.reserve(items.size());
  for (std::size_t index = 0U; index < items.size(); ++index) {
    const std::string item_path = std::string(path) + "[" + std::to_string(index) + "]";
    const std::string& item = require_string(items[index], item_path);
    if (!is_portable_token(item)) {
      fail(item_path, "expected stable portable compatibility ID");
    }
    parsed.push_back(item);
  }
  if (!std::is_sorted(parsed.begin(), parsed.end()) ||
      std::adjacent_find(parsed.begin(), parsed.end()) != parsed.end()) {
    fail(path, "must be duplicate-free and canonical-sorted");
  }
}

void validate_compatibility(const CanonicalValue& value) {
  const auto& compatibility = require_object(value, "$.compatibility");
  const std::initializer_list<std::string_view> kFields{"lane", "provides", "tags"};
  for (const auto& [name, unused] : compatibility) {
    (void)unused;
    if (!contains(kFields, name)) {
      fail("$.compatibility", "unknown field '" + name + "'");
    }
  }
  for (const std::string_view field : kFields) {
    (void)require_member(compatibility, field, "$.compatibility");
  }
  const std::string& lane = require_string(require_member(compatibility, "lane", "$.compatibility"),
                                           "$.compatibility.lane");
  if (!contains({"manyforge-only", "isaac-sim", "real-robot"}, lane)) {
    fail("$.compatibility.lane", "unsupported runtime lane");
  }
  validate_compatibility_set(require_member(compatibility, "provides", "$.compatibility"),
                             "$.compatibility.provides");
  validate_compatibility_set(require_member(compatibility, "tags", "$.compatibility"),
                             "$.compatibility.tags");
}

[[nodiscard]] CanonicalValue make_synthetic_graph_root(
    const CanonicalValue::Object& authored_graph,
    const core::CellId& cell_id) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(core::kCellGraphApiVersion))},
      {"graph_revision", CanonicalValue(std::string(kPlaceholderDigest))},
      {"cell_id", CanonicalValue(cell_id.value())},
      {"runtime_units",
       require_member(authored_graph, "runtime_units", "$.graph")},
      {"model_instances",
       require_member(authored_graph, "model_instances", "$.graph")},
      {"controllable_components",
       require_member(authored_graph, "controllable_components", "$.graph")},
      {"control_domains",
       require_member(authored_graph, "control_domains", "$.graph")},
      {"control_bindings",
       require_member(authored_graph, "control_bindings", "$.graph")},
      {"component_attachments",
       require_member(authored_graph, "component_attachments", "$.graph")},
  });
}

[[nodiscard]] CanonicalValue authored_graph_member(
    const CanonicalValue& normalized_full_graph) {
  const auto& graph = require_object(normalized_full_graph, "$.graph");
  return CanonicalValue(CanonicalValue::Object{
      {"runtime_units",
       require_member(graph, "runtime_units", "$.graph")},
      {"model_instances",
       require_member(graph, "model_instances", "$.graph")},
      {"controllable_components",
       require_member(graph, "controllable_components", "$.graph")},
      {"control_domains",
       require_member(graph, "control_domains", "$.graph")},
      {"control_bindings",
       require_member(graph, "control_bindings", "$.graph")},
      {"component_attachments",
       require_member(graph, "component_attachments", "$.graph")},
  });
}

struct DecodedDeployment final {
  core::DeploymentArtifactId deployment_artifact_id;
  core::DeploymentRevision declared_deployment_revision;
  core::CellId cell_id;
  std::string name;
  std::optional<std::string> description;
  std::optional<CanonicalValue> compatibility;
  std::vector<core::CatalogRecordRefDTO> catalog_records;
  GraphIdentityReadyCellGraphCandidate graph_candidate;
  CanonicalValue normalized_authored_graph;
  TypedDeploymentApplicabilityInputsCandidate applicability_candidate;
};

[[nodiscard]] DecodedDeployment decode_deployment(
    const ParsedArtifactCandidate& parsed_deployment,
    std::span<const QualifiedRuntimeProfile> qualified_runtime_profiles) {
  const auto& root = require_object(parsed_deployment.root(), "$");
  require_closed_root(root);

  const std::string& api_version =
      require_string(require_member(root, "api_version", "$"),
                     "$.api_version");
  if (api_version != kDeploymentApiVersion) {
    fail("$.api_version", "expected manyforge.deployment.v1");
  }

  std::optional<CanonicalValue> compatibility;
  if (const CanonicalValue* authored = find_member(root, "compatibility"); authored != nullptr) {
    validate_compatibility(*authored);
    compatibility = *authored;
  }

  std::optional<std::string> description;
  if (const CanonicalValue* authored_description =
          find_member(root, "description");
      authored_description != nullptr) {
    description =
        require_string(*authored_description, "$.description");
  }

  const core::DeploymentArtifactId deployment_artifact_id =
      parse_portable_id<core::DeploymentArtifactId>(
          require_member(root, "deployment_artifact_id", "$"),
          "$.deployment_artifact_id");
  const core::DeploymentRevision deployment_revision =
      parse_digest<core::DeploymentRevision>(
          require_member(root, "deployment_revision", "$"),
          "$.deployment_revision");
  const core::CellId cell_id = parse_portable_id<core::CellId>(
      require_member(root, "cell_id", "$"), "$.cell_id");
  const std::string name =
      require_string(require_member(root, "name", "$"), "$.name");

  const auto& authored_graph =
      require_object(require_member(root, "graph", "$"), "$.graph");
  require_exact_graph_fields(authored_graph);
  const CanonicalValue synthetic_graph =
      make_synthetic_graph_root(authored_graph, cell_id);
  const ParsedArtifactCandidate parsed_graph =
      parse_strict_json_model_yaml(canonical_json(synthetic_graph));
  const TypedCellGraphCandidate typed_graph =
      decode_cell_graph_candidate(parsed_graph);
  const IntrinsicallyValidatedCellGraphCandidate intrinsic_graph =
      validate_intrinsic_cell_graph_candidate(typed_graph);
  const GraphIdentityReadyCellGraphCandidate identity_ready_graph =
      prepare_cell_graph_identity(intrinsic_graph,
                                  qualified_runtime_profiles);
  const CanonicalValue normalized_authored_graph =
      authored_graph_member(identity_ready_graph.normalized_root());

  std::vector<core::CatalogRecordRefDTO> catalog_records;
  try {
    catalog_records = detail::decode_deployment_catalogs(
        require_member(root, "catalogs", "$"));
  } catch (const detail::DeploymentCatalogCodecError& error) {
    fail("$.catalogs", error.what());
  }
  for (const auto& record : catalog_records) {
    switch (core::catalog_record_kind(record)) {
      case core::CatalogRecordKind::kModelAsset:
      case core::CatalogRecordKind::kRuntimeProfile:
      case core::CatalogRecordKind::kComponentType:
      case core::CatalogRecordKind::kControlInterfaceContract:
      case core::CatalogRecordKind::kControllerTopology:
      case core::CatalogRecordKind::kProviderManifest:
      case core::CatalogRecordKind::kCommandSchema:
      case core::CatalogRecordKind::kCapabilityDefinition:
      case core::CatalogRecordKind::kStopEvidencePolicy:
        break;
      case core::CatalogRecordKind::kContactPolicy:
      case core::CatalogRecordKind::kRuntimePolicy:
        fail("$.catalogs.records",
             "catalog record kind '" +
                 std::string(core::to_string(
                     core::catalog_record_kind(record))) +
                 "' is not qualified by the current compiler subset");
    }
  }

  const TypedDeploymentApplicabilityInputsCandidate applicability_candidate =
      decode_deployment_applicability_inputs_candidate(parsed_deployment);

  return {
      deployment_artifact_id,
      deployment_revision,
      cell_id,
      name,
      std::move(description),
      std::move(compatibility),
      std::move(catalog_records),
      identity_ready_graph,
      normalized_authored_graph,
      applicability_candidate,
  };
}

[[nodiscard]] CanonicalValue normalized_deployment_root(
    const DecodedDeployment& decoded,
    const core::DeploymentRevision& deployment_revision,
    const CanonicalValue& normalized_graph,
    const RevisionVerifiedDeploymentApplicabilityInputs& applicability) {
  CanonicalValue::Object root{
      {"api_version", CanonicalValue(std::string(kDeploymentApiVersion))},
      {"deployment_artifact_id",
       CanonicalValue(decoded.deployment_artifact_id.value())},
      {"deployment_revision",
       CanonicalValue(deployment_revision.value())},
      {"cell_id", CanonicalValue(decoded.cell_id.value())},
      {"name", CanonicalValue(decoded.name)},
      {"catalogs",
       detail::encode_deployment_catalogs(decoded.catalog_records)},
      {"graph", normalized_graph},
      {"desired_control_configurations",
       applicability.normalized_desired_control_configurations()},
      {"capability_provider_bindings",
       applicability.normalized_capability_provider_bindings()},
  };
  if (decoded.description.has_value()) {
    root.emplace_back("description",
                      CanonicalValue(*decoded.description));
  }
  if (decoded.compatibility.has_value()) {
    root.emplace_back("compatibility", *decoded.compatibility);
  }

  switch (applicability.site_overlay_ref_presence()) {
    case SiteOverlayRefSourcePresence::kAbsent:
      break;
    case SiteOverlayRefSourcePresence::kExplicitNull:
    case SiteOverlayRefSourcePresence::kValue:
      root.emplace_back(
          "site_overlay_ref",
          *applicability.normalized_site_overlay_ref());
      break;
  }
  if (applicability.external_resources_was_present()) {
    root.emplace_back("external_resources",
                      applicability.normalized_external_resources());
  }
  if (applicability.policies_was_present()) {
    root.emplace_back("policies", *applicability.normalized_policies());
  }
  if (applicability.assistant_settings().source_was_present()) {
    root.emplace_back("assistant",
                      applicability.assistant_settings().normalized_source());
  }
  return CanonicalValue(std::move(root));
}

struct VerifiedEnvelopeParts final {
  DecodedDeployment decoded;
  core::DeploymentRevision deployment_revision;
  GraphRevisionVerifiedCellGraph graph;
  RevisionVerifiedDeploymentApplicabilityInputs applicability;
  CanonicalValue normalized_root;
};

[[nodiscard]] VerifiedEnvelopeParts compute_parts(
    const ParsedArtifactCandidate& parsed_deployment,
    std::span<const QualifiedRuntimeProfile> qualified_runtime_profiles) {
  DecodedDeployment decoded =
      decode_deployment(parsed_deployment, qualified_runtime_profiles);
  GraphRevisionVerifiedCellGraph verified_graph =
      compute_graph_revision(decoded.graph_candidate);
  const CanonicalValue normalized_graph =
      authored_graph_member(verified_graph.normalized_root());
  RevisionVerifiedDeploymentApplicabilityInputs applicability =
      compute_capability_provider_binding_revisions(
          decoded.applicability_candidate);

  core::DeploymentRevision deployment_revision{
      std::string(kPlaceholderDigest)};
  CanonicalValue normalized_root = normalized_deployment_root(
      decoded, deployment_revision, normalized_graph, applicability);
  detail::validate_canonical_value_output_budget(normalized_root);
  deployment_revision = core::DeploymentRevision(
      detail::compute_content_identity(normalized_root,
                                       kDeploymentIdentitySpec));
  normalized_root = normalized_deployment_root(
      decoded, deployment_revision, normalized_graph, applicability);
  detail::validate_canonical_value_output_budget(normalized_root);

  return {std::move(decoded), std::move(deployment_revision),
          std::move(verified_graph), std::move(applicability),
          std::move(normalized_root)};
}

[[nodiscard]] VerifiedEnvelopeParts verify_parts(
    const ParsedArtifactCandidate& parsed_deployment,
    std::span<const QualifiedRuntimeProfile> qualified_runtime_profiles) {
  DecodedDeployment decoded =
      decode_deployment(parsed_deployment, qualified_runtime_profiles);

  // The authored deployment graph intentionally has no outer GraphRevision,
  // so the public graph verifier cannot consume it directly. Recompute the
  // complete expected graph through the ordinary graph producer, compare the
  // exact six-array authored member before accepting, and only then retain
  // the producer's derived outer GraphRevision.
  GraphRevisionVerifiedCellGraph expected_graph =
      compute_graph_revision(decoded.graph_candidate);
  const CanonicalValue expected_authored_graph =
      authored_graph_member(expected_graph.normalized_root());
  if (decoded.normalized_authored_graph != expected_authored_graph) {
    fail("$.graph",
         "a graph-owned nested revision does not match the independently "
         "recomputed exact record");
  }

  RevisionVerifiedDeploymentApplicabilityInputs applicability =
      verify_capability_provider_binding_revisions(
          decoded.applicability_candidate);
  CanonicalValue normalized_root = normalized_deployment_root(
      decoded, decoded.declared_deployment_revision,
      decoded.normalized_authored_graph, applicability);
  detail::validate_canonical_value_output_budget(normalized_root);
  if (!detail::verify_content_identity(normalized_root,
                                       kDeploymentIdentitySpec)) {
    fail("$.deployment_revision",
         "deployment_revision does not match the exact normalized "
         "deployment");
  }

  core::DeploymentRevision deployment_revision =
      std::move(decoded.declared_deployment_revision);
  return {std::move(decoded), std::move(deployment_revision),
          std::move(expected_graph), std::move(applicability),
          std::move(normalized_root)};
}

template <typename Operation>
[[nodiscard]] auto translate_errors(Operation&& operation)
    -> decltype(operation()) {
  try {
    return operation();
  } catch (const DeploymentArtifactCodecError&) {
    throw;
  } catch (const std::invalid_argument& error) {
    throw DeploymentArtifactCodecError(
        std::string("deployment envelope: ") + error.what());
  }
}

}  // namespace

RevisionVerifiedDeploymentArtifact compute_deployment_artifact_revision(
    const ParsedArtifactCandidate& parsed_deployment,
    std::span<const QualifiedRuntimeProfile> qualified_runtime_profiles) {
  VerifiedEnvelopeParts parts = translate_errors([&] {
    return compute_parts(parsed_deployment, qualified_runtime_profiles);
  });
  return RevisionVerifiedDeploymentArtifact(
      std::move(parts.decoded.deployment_artifact_id),
      std::move(parts.deployment_revision),
      std::move(parts.decoded.cell_id), std::move(parts.decoded.name),
      std::move(parts.decoded.description),
      std::move(parts.decoded.catalog_records), parts.graph,
      parts.applicability, std::move(parts.normalized_root));
}

RevisionVerifiedDeploymentArtifact verify_deployment_artifact_revision(
    const ParsedArtifactCandidate& parsed_deployment,
    std::span<const QualifiedRuntimeProfile> qualified_runtime_profiles) {
  VerifiedEnvelopeParts parts = translate_errors([&] {
    return verify_parts(parsed_deployment, qualified_runtime_profiles);
  });
  return RevisionVerifiedDeploymentArtifact(
      std::move(parts.decoded.deployment_artifact_id),
      std::move(parts.deployment_revision),
      std::move(parts.decoded.cell_id), std::move(parts.decoded.name),
      std::move(parts.decoded.description),
      std::move(parts.decoded.catalog_records), parts.graph,
      parts.applicability, std::move(parts.normalized_root));
}

}  // namespace manyforge::deployment
