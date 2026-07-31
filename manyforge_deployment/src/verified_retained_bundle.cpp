#include "manyforge_deployment/verified_retained_bundle.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <set>
#include <string>
#include <utility>

#include "compiler_semantic_limits_internal.hpp"
#include "content_identity_internal.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/cell_graph_codec.hpp"
#include "manyforge_deployment/closed_schema.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kFlattenedModelContractId =
    "manyforge.flattened_model";
constexpr std::string_view kFlattenedModelPlacement =
    "cell_planning_collision_model";
constexpr std::string_view kContentIdentityPrefix =
    "manyforge.content-identity.v1";
constexpr std::string_view kRetainedProductOutputDomain =
    "retained-product-output";
constexpr std::string_view kRetainedProductSchemaTag =
    "manyforge.retained_bundle.v1#retained_product";
constexpr std::size_t kDigestHexLength = 64U;
constexpr std::string_view kDigestPrefix = "sha256:";
constexpr std::string_view kStoreKeyPrefix = "sha256/";

constexpr std::array<detail::QualifiedRetainedProductArtifactRole, 2U>
    kFlattenedModelArtifactRoles{{
        {"srdf", "application/xml", true},
        {"urdf", "application/xml", true},
    }};

[[nodiscard]] constexpr bool
is_ascii_alphanumeric(unsigned char byte) noexcept {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9');
}

[[nodiscard]] bool is_portable_identity(std::string_view value) noexcept {
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

[[nodiscard]] bool is_lower_hex(std::string_view value) noexcept {
  return !value.empty() &&
         std::all_of(value.begin(), value.end(), [](unsigned char byte) {
           return (byte >= '0' && byte <= '9') ||
                  (byte >= 'a' && byte <= 'f');
         });
}

[[nodiscard]] const CanonicalValue::Object&
require_object(const CanonicalValue& value, std::string_view field) {
  const auto* object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    detail::fail_retained_bundle(std::string(field) + " must be an object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue::Array&
require_array(const CanonicalValue& value, std::string_view field) {
  const auto* array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    detail::fail_retained_bundle(std::string(field) + " must be an array");
  }
  return *array;
}

[[nodiscard]] const std::string&
require_string(const CanonicalValue& value, std::string_view field) {
  const auto* text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    detail::fail_retained_bundle(std::string(field) + " must be a string");
  }
  return *text;
}

[[nodiscard]] const CanonicalValue&
require_member(const CanonicalValue::Object& object, std::string_view name,
               std::string_view field) {
  const auto found = std::find_if(
      object.begin(), object.end(),
      [name](const auto& member) { return member.first == name; });
  if (found == object.end()) {
    detail::fail_retained_bundle(std::string(field) +
                                 " is missing required member " +
                                 std::string(name));
  }
  return found->second;
}

void require_exact_fields(
    const CanonicalValue::Object& object,
    std::initializer_list<std::string_view> expected,
    std::string_view field) {
  if (object.size() != expected.size()) {
    detail::fail_retained_bundle(std::string(field) +
                                 " has an incorrect closed field count");
  }
  for (const auto& [name, unused] : object) {
    (void)unused;
    if (std::find(expected.begin(), expected.end(), name) == expected.end()) {
      detail::fail_retained_bundle(std::string(field) +
                                   " contains unknown member " + name);
    }
  }
  for (const std::string_view name : expected) {
    (void)require_member(object, name, field);
  }
}

void require_empty_array(const CanonicalValue& value,
                         std::string_view field) {
  if (!require_array(value, field).empty()) {
    detail::fail_retained_bundle(
        std::string(field) +
        " is outside the currently qualified canonical-empty subset");
  }
}

void require_null(const CanonicalValue& value, std::string_view field) {
  if (!std::holds_alternative<std::nullptr_t>(value.storage())) {
    detail::fail_retained_bundle(std::string(field) + " must be null");
  }
}

[[nodiscard]] std::uint64_t require_i_json_size(
    const CanonicalValue& value, std::string_view field) {
  const auto* number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number) || *number < 0.0 ||
      std::trunc(*number) != *number ||
      *number > static_cast<double>(detail::kMaximumIJsonInteger)) {
    detail::fail_retained_bundle(
        std::string(field) +
        " must be a non-negative I-JSON safe integer");
  }
  return static_cast<std::uint64_t>(*number);
}

[[nodiscard]] CanonicalValue artifact_role_projection(
    const detail::QualifiedRetainedProductArtifactRole& role) {
  return CanonicalValue(CanonicalValue::Object{
      {"artifact_role_id", CanonicalValue(std::string(role.artifact_role_id))},
      {"media_type", CanonicalValue(std::string(role.media_type))},
      {"cardinality",
       CanonicalValue(role.required ? "required_exactly_one"
                                    : "optional_at_most_one")},
  });
}

[[nodiscard]] CanonicalValue product_contract_projection(
    std::string_view revision) {
  CanonicalValue::Array roles;
  roles.reserve(kFlattenedModelArtifactRoles.size());
  for (const auto& role : kFlattenedModelArtifactRoles) {
    roles.push_back(artifact_role_projection(role));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(core::kProductContractApiVersion))},
      {"product_contract_id",
       CanonicalValue(std::string(kFlattenedModelContractId))},
      {"product_contract_revision", CanonicalValue(std::string(revision))},
      {"product_kind",
       CanonicalValue(std::string(core::to_string(
           core::RetainedProductKind::kPlanningCollisionModel)))},
      {"placement", CanonicalValue(std::string(kFlattenedModelPlacement))},
      {"artifact_roles", CanonicalValue(std::move(roles))},
  });
}

struct QualifiedProductContractState final {
  core::ProductContractRevision revision;
  CanonicalValue descriptor;
};

[[nodiscard]] const QualifiedProductContractState&
qualified_product_contract_state() {
  static const QualifiedProductContractState state = [] {
    const std::string revision = detail::compute_content_identity(
        product_contract_projection(kZeroDigest),
        detail::ContentIdentitySpec{
            .identity_domain = "product-contract-revision",
            .api_version = std::string(core::kProductContractApiVersion),
            .own_field = "product_contract_revision",
        });
    CanonicalValue descriptor = product_contract_projection(revision);
    if (!detail::verify_content_identity(
            descriptor,
            detail::ContentIdentitySpec{
                .identity_domain = "product-contract-revision",
                .api_version = std::string(core::kProductContractApiVersion),
                .own_field = "product_contract_revision",
            })) {
      throw std::logic_error(
          "built-in product contract has an invalid content identity");
    }
    return QualifiedProductContractState{
        core::ProductContractRevision(revision), std::move(descriptor)};
  }();
  return state;
}

[[nodiscard]] CanonicalValue retained_artifact_projection(
    const core::RetainedArtifactRefDTO& artifact) {
  return CanonicalValue(CanonicalValue::Object{
      {"artifact_role_id", CanonicalValue(artifact.artifact_role_id.value())},
      {"media_type", CanonicalValue(artifact.media_type)},
      {"byte_size", CanonicalValue(static_cast<double>(artifact.byte_size))},
      {"content_digest", CanonicalValue(artifact.content_digest.value())},
      {"content_addressed_store_key",
       CanonicalValue(artifact.content_addressed_store_key.value())},
  });
}

[[nodiscard]] std::string domain_separated_canonical_digest(
    std::string_view domain, std::string_view schema_tag,
    const CanonicalValue& value) {
  std::string preimage;
  const std::string payload = canonical_json(value);
  preimage.reserve(kContentIdentityPrefix.size() + domain.size() +
                   schema_tag.size() + payload.size() + 3U);
  preimage.append(kContentIdentityPrefix);
  preimage.push_back('\0');
  preimage.append(domain);
  preimage.push_back('\0');
  preimage.append(schema_tag);
  preimage.push_back('\0');
  preimage.append(payload);
  return sha256_digest(preimage);
}

[[nodiscard]] core::RetainedProductDTO parse_retained_product(
    const CanonicalValue& value, std::string_view field) {
  const auto& object = require_object(value, field);
  require_exact_fields(object,
                       {"product_id", "product_kind",
                        "product_contract_id", "product_contract_revision",
                        "artifact_refs"},
                       field);

  const std::string& product_id =
      require_string(require_member(object, "product_id", field),
                     "compiled_products.planning_collision_model.product_id");
  const std::string& product_kind =
      require_string(require_member(object, "product_kind", field),
                     "compiled_products.planning_collision_model.product_kind");
  if (product_kind !=
      core::to_string(core::RetainedProductKind::kPlanningCollisionModel)) {
    detail::fail_retained_bundle(
        "canonical-empty planning product has an invalid product_kind");
  }
  const std::string& contract_id = require_string(
      require_member(object, "product_contract_id", field),
      "compiled_products.planning_collision_model.product_contract_id");
  const std::string& contract_revision = require_string(
      require_member(object, "product_contract_revision", field),
      "compiled_products.planning_collision_model."
      "product_contract_revision");
  detail::require_retained_digest(contract_revision,
                                  "product_contract_revision");

  std::vector<core::RetainedArtifactRefDTO> artifacts;
  const auto& artifact_values =
      require_array(require_member(object, "artifact_refs", field),
                    "compiled_products.planning_collision_model.artifact_refs");
  artifacts.reserve(artifact_values.size());
  for (std::size_t index = 0U; index < artifact_values.size(); ++index) {
    const std::string path =
        "compiled_products.planning_collision_model.artifact_refs[" +
        std::to_string(index) + "]";
    const auto& artifact = require_object(artifact_values[index], path);
    require_exact_fields(
        artifact,
        {"artifact_role_id", "media_type", "byte_size", "content_digest",
         "content_addressed_store_key"},
        path);
    artifacts.push_back(core::RetainedArtifactRefDTO{
        core::ArtifactRoleId(require_string(
            require_member(artifact, "artifact_role_id", path),
            path + ".artifact_role_id")),
        require_string(require_member(artifact, "media_type", path),
                       path + ".media_type"),
        require_i_json_size(require_member(artifact, "byte_size", path),
                            path + ".byte_size"),
        core::ContentDigest(require_string(
            require_member(artifact, "content_digest", path),
            path + ".content_digest")),
        core::ContentAddressedStoreKey(require_string(
            require_member(artifact, "content_addressed_store_key", path),
            path + ".content_addressed_store_key")),
    });
  }

  core::RetainedProductDTO product{
      core::ProductId(product_id),
      core::RetainedProductKind::kPlanningCollisionModel,
      core::ProductContractId(contract_id),
      core::ProductContractRevision(contract_revision),
      std::move(artifacts),
  };
  detail::require_current_retained_product_conformance(product);
  if (detail::retained_product_projection(product) != value) {
    detail::fail_retained_bundle(
        "planning product differs from its exact typed projection");
  }
  return product;
}

[[nodiscard]] GraphRevisionVerifiedCellGraph
verify_canonical_empty_graph_projection(const CanonicalValue& graph_value) {
  const auto typed = decode_cell_graph_candidate(
      parse_strict_json_model_yaml(canonical_json(graph_value)));
  const auto intrinsic = validate_intrinsic_cell_graph_candidate(typed);
  const auto ready = prepare_cell_graph_identity(
      intrinsic, std::span<const QualifiedRuntimeProfile>{});
  const auto verified = verify_graph_revision(ready);
  if (verified.normalized_root() != graph_value) {
    detail::fail_retained_bundle(
        "retained cell_graph differs from its exact verified projection");
  }
  const auto& graph = verified.graph();
  if (!graph.runtime_units.empty() || !graph.model_instances.empty() ||
      !graph.controllable_components.empty() ||
      !graph.control_domains.empty() || !graph.control_bindings.empty() ||
      !graph.component_attachments.empty()) {
    detail::fail_retained_bundle(
        "retained cell_graph is outside the canonical-empty subset");
  }
  return verified;
}

void verify_node_environment_projection(const CanonicalValue& value) {
  const auto& object = require_object(value, "catalogs.node_environment");
  require_exact_fields(
      object,
      {"catalog_id", "content_version", "catalog_digest",
       "environment_digest"},
      "catalogs.node_environment");
  detail::require_retained_portable_identity(
      require_string(require_member(object, "catalog_id",
                                    "catalogs.node_environment"),
                     "catalogs.node_environment.catalog_id"),
      "catalogs.node_environment.catalog_id");
  detail::require_retained_portable_identity(
      require_string(require_member(object, "content_version",
                                    "catalogs.node_environment"),
                     "catalogs.node_environment.content_version"),
      "catalogs.node_environment.content_version");
  detail::require_retained_digest(
      require_string(require_member(object, "catalog_digest",
                                    "catalogs.node_environment"),
                     "catalogs.node_environment.catalog_digest"),
      "catalogs.node_environment.catalog_digest");
  detail::require_retained_digest(
      require_string(require_member(object, "environment_digest",
                                    "catalogs.node_environment"),
                     "catalogs.node_environment.environment_digest"),
      "catalogs.node_environment.environment_digest");
}

[[nodiscard]] core::RetainedBundleDTO
verify_current_retained_bundle_projection(const CanonicalValue& root) {
  const auto& object = require_object(root, "retained_bundle");
  require_exact_fields(
      object,
      {"api_version", "retained_bundle_id", "deployment_artifact_id",
       "deployment_revision", "cell_graph", "catalogs",
       "desired_control_configurations", "capability_provider_bindings",
       "compiled_products", "provenance"},
      "retained_bundle");
  if (require_string(require_member(object, "api_version", "retained_bundle"),
                     "retained_bundle.api_version") !=
      core::kRetainedBundleApiVersion) {
    detail::fail_retained_bundle(
        "retained_bundle.api_version is not the exact v1 API");
  }
  const std::string& retained_bundle_id = require_string(
      require_member(object, "retained_bundle_id", "retained_bundle"),
      "retained_bundle.retained_bundle_id");
  detail::require_retained_digest(retained_bundle_id,
                                  "retained_bundle.retained_bundle_id");
  const std::string& deployment_artifact_id = require_string(
      require_member(object, "deployment_artifact_id", "retained_bundle"),
      "retained_bundle.deployment_artifact_id");
  detail::require_retained_portable_identity(
      deployment_artifact_id, "retained_bundle.deployment_artifact_id");
  const std::string& deployment_revision = require_string(
      require_member(object, "deployment_revision", "retained_bundle"),
      "retained_bundle.deployment_revision");
  detail::require_retained_digest(deployment_revision,
                                  "retained_bundle.deployment_revision");
  const auto verified_graph = verify_canonical_empty_graph_projection(
      require_member(object, "cell_graph", "retained_bundle"));

  const auto& catalogs =
      require_object(require_member(object, "catalogs", "retained_bundle"),
                     "retained_bundle.catalogs");
  require_exact_fields(
      catalogs,
      {"dialect_api_version", "bootstrap_contract_digest",
       "qualification_registry_digest", "records", "schema_artifacts",
       "node_environment"},
      "retained_bundle.catalogs");
  const std::string& dialect_api_version = require_string(
      require_member(catalogs, "dialect_api_version",
                     "retained_bundle.catalogs"),
      "retained_bundle.catalogs.dialect_api_version");
  if (dialect_api_version != kClosedSchemaApiVersion) {
    detail::fail_retained_bundle(
        "retained catalog dialect does not match this build");
  }
  const std::string& bootstrap_contract_digest = require_string(
      require_member(catalogs, "bootstrap_contract_digest",
                     "retained_bundle.catalogs"),
      "retained_bundle.catalogs.bootstrap_contract_digest");
  if (bootstrap_contract_digest != kClosedSchemaBootstrapContractDigest) {
    detail::fail_retained_bundle(
        "retained bootstrap contract does not match this build");
  }
  const std::string& qualification_registry_digest = require_string(
      require_member(catalogs, "qualification_registry_digest",
                     "retained_bundle.catalogs"),
      "retained_bundle.catalogs.qualification_registry_digest");
  if (qualification_registry_digest !=
      build_qualified_closed_schema_registry()
          .qualification_registry_digest()
          .value()) {
    detail::fail_retained_bundle(
        "retained qualification registry does not match this build");
  }
  require_empty_array(require_member(catalogs, "records",
                                     "retained_bundle.catalogs"),
                      "retained_bundle.catalogs.records");
  require_empty_array(require_member(catalogs, "schema_artifacts",
                                     "retained_bundle.catalogs"),
                      "retained_bundle.catalogs.schema_artifacts");
  const CanonicalValue& node_environment_value = require_member(
      catalogs, "node_environment", "retained_bundle.catalogs");
  verify_node_environment_projection(node_environment_value);
  const auto& node_environment =
      require_object(node_environment_value, "catalogs.node_environment");

  require_empty_array(
      require_member(object, "desired_control_configurations",
                     "retained_bundle"),
      "retained_bundle.desired_control_configurations");
  require_empty_array(
      require_member(object, "capability_provider_bindings",
                     "retained_bundle"),
      "retained_bundle.capability_provider_bindings");

  const auto& products = require_object(
      require_member(object, "compiled_products", "retained_bundle"),
      "retained_bundle.compiled_products");
  require_exact_fields(
      products,
      {"planning_collision_model", "viewer_model",
       "control_domain_products", "symbol_role_maps",
       "runtime_unit_products"},
      "retained_bundle.compiled_products");
  const core::RetainedProductDTO planning = parse_retained_product(
      require_member(products, "planning_collision_model",
                     "retained_bundle.compiled_products"),
      "retained_bundle.compiled_products.planning_collision_model");
  require_null(require_member(products, "viewer_model",
                              "retained_bundle.compiled_products"),
               "retained_bundle.compiled_products.viewer_model");
  require_empty_array(
      require_member(products, "control_domain_products",
                     "retained_bundle.compiled_products"),
      "retained_bundle.compiled_products.control_domain_products");
  require_empty_array(
      require_member(products, "symbol_role_maps",
                     "retained_bundle.compiled_products"),
      "retained_bundle.compiled_products.symbol_role_maps");
  require_empty_array(
      require_member(products, "runtime_unit_products",
                     "retained_bundle.compiled_products"),
      "retained_bundle.compiled_products.runtime_unit_products");

  const auto& provenance =
      require_object(require_member(object, "provenance", "retained_bundle"),
                     "retained_bundle.provenance");
  require_exact_fields(
      provenance,
      {"compiler_contract_digest", "compiler_build_digest",
       "source_records", "generator_records", "output_records"},
      "retained_bundle.provenance");
  const std::string& compiler_contract_digest = require_string(
      require_member(provenance, "compiler_contract_digest",
                     "retained_bundle.provenance"),
      "retained_bundle.provenance.compiler_contract_digest");
  detail::require_retained_digest(
      compiler_contract_digest,
      "retained_bundle.provenance.compiler_contract_digest");
  const std::string& compiler_build_digest = require_string(
      require_member(provenance, "compiler_build_digest",
                     "retained_bundle.provenance"),
      "retained_bundle.provenance.compiler_build_digest");
  detail::require_retained_digest(
      compiler_build_digest,
      "retained_bundle.provenance.compiler_build_digest");
  require_empty_array(require_member(provenance, "source_records",
                                     "retained_bundle.provenance"),
                      "retained_bundle.provenance.source_records");
  require_empty_array(require_member(provenance, "generator_records",
                                     "retained_bundle.provenance"),
                      "retained_bundle.provenance.generator_records");

  const auto& outputs =
      require_array(require_member(provenance, "output_records",
                                   "retained_bundle.provenance"),
                    "retained_bundle.provenance.output_records");
  if (outputs.size() != 1U) {
    detail::fail_retained_bundle(
        "canonical-empty provenance must contain exactly one output record");
  }
  const auto& output = require_object(
      outputs.front(), "retained_bundle.provenance.output_records[0]");
  require_exact_fields(output, {"product_id", "product_kind", "content_digest"},
                       "retained_bundle.provenance.output_records[0]");
  if (require_string(require_member(output, "product_id",
                                    "retained_bundle.provenance."
                                    "output_records[0]"),
                     "retained_bundle.provenance.output_records[0]."
                     "product_id") != planning.product_id.value() ||
      require_string(require_member(output, "product_kind",
                                    "retained_bundle.provenance."
                                    "output_records[0]"),
                     "retained_bundle.provenance.output_records[0]."
                     "product_kind") !=
          core::to_string(planning.product_kind) ||
      require_string(require_member(output, "content_digest",
                                    "retained_bundle.provenance."
                                    "output_records[0]"),
                     "retained_bundle.provenance.output_records[0]."
                     "content_digest") !=
          detail::retained_product_output_digest(planning).value()) {
    detail::fail_retained_bundle(
        "retained output record does not match the complete planning "
        "product projection");
  }
  return core::RetainedBundleDTO{
      std::string(core::kRetainedBundleApiVersion),
      core::RetainedBundleId(retained_bundle_id),
      core::DeploymentArtifactId(deployment_artifact_id),
      core::DeploymentRevision(deployment_revision),
      verified_graph.graph(),
      core::RetainedCatalogDTO{
          dialect_api_version,
          core::ContentDigest(bootstrap_contract_digest),
          core::ContentDigest(qualification_registry_digest),
          {},
          {},
          core::RetainedNodeEnvironmentDTO{
              require_string(
                  require_member(node_environment, "catalog_id",
                                 "catalogs.node_environment"),
                  "catalogs.node_environment.catalog_id"),
              require_string(
                  require_member(node_environment, "content_version",
                                 "catalogs.node_environment"),
                  "catalogs.node_environment.content_version"),
              core::ContentDigest(require_string(
                  require_member(node_environment, "catalog_digest",
                                 "catalogs.node_environment"),
                  "catalogs.node_environment.catalog_digest")),
              core::ContentDigest(require_string(
                  require_member(node_environment, "environment_digest",
                                 "catalogs.node_environment"),
                  "catalogs.node_environment.environment_digest")),
          },
      },
      {},
      {},
      core::RetainedCompiledProductsDTO{
          planning,
          std::nullopt,
          {},
          {},
          {},
      },
      core::RetainedBundleProvenanceDTO{
          core::ContentDigest(compiler_contract_digest),
          core::ContentDigest(compiler_build_digest),
          {},
          {},
          {core::RetainedOutputRecordDTO{
              planning.product_id,
              planning.product_kind,
              detail::retained_product_output_digest(planning),
          }},
      },
  };
}

}  // namespace

namespace detail {

std::string_view flattened_model_product_contract_id() noexcept {
  return kFlattenedModelContractId;
}

const core::ProductContractRevision&
flattened_model_product_contract_revision() {
  return qualified_product_contract_state().revision;
}

std::span<const QualifiedRetainedProductArtifactRole>
flattened_model_product_artifact_roles() noexcept {
  return kFlattenedModelArtifactRoles;
}

const CanonicalValue& flattened_model_product_contract_descriptor() {
  return qualified_product_contract_state().descriptor;
}

[[noreturn]] void fail_retained_bundle(std::string_view message) {
  throw RetainedBundleError(std::string(message));
}

void require_retained_portable_identity(std::string_view value,
                                        std::string_view field) {
  if (!is_portable_identity(value)) {
    fail_retained_bundle(
        std::string(field) +
        " must use the portable 1..128-byte identity grammar");
  }
}

void require_retained_digest(std::string_view value, std::string_view field) {
  if (!is_sha256_digest(value)) {
    fail_retained_bundle(
        std::string(field) +
        " must be an exact lowercase sha256:<64 hex> digest");
  }
}

std::string retained_store_key_for_digest(std::string_view digest) {
  require_retained_digest(digest, "content_digest");
  return std::string(kStoreKeyPrefix) +
         std::string(digest.substr(kDigestPrefix.size()));
}

std::optional<std::string_view>
retained_digest_hex(std::string_view digest) noexcept {
  if (!is_sha256_digest(digest)) {
    return std::nullopt;
  }
  return digest.substr(kDigestPrefix.size());
}

bool is_retained_digest_hex(std::string_view hex) noexcept {
  return hex.size() == kDigestHexLength && is_lower_hex(hex);
}

std::string retained_digest_from_hex(std::string_view hex) {
  if (!is_retained_digest_hex(hex)) {
    fail_retained_bundle(
        "durable retention entry has an invalid digest filename");
  }
  return std::string(kDigestPrefix) + std::string(hex);
}

std::optional<std::string_view> retained_raw_key_hex(
    const core::ContentAddressedStoreKey& key,
    const core::ContentDigest& expected_digest) noexcept {
  const auto hex = retained_digest_hex(expected_digest.value());
  if (!hex.has_value() ||
      key.value() != std::string(kStoreKeyPrefix) + std::string(*hex)) {
    return std::nullopt;
  }
  return hex;
}

void require_current_retained_product_conformance(
    const core::RetainedProductDTO& product) {
  require_retained_portable_identity(product.product_id.value(), "product_id");
  if (product.product_contract_id.value() != kFlattenedModelContractId ||
      product.product_contract_revision !=
          flattened_model_product_contract_revision() ||
      product.product_kind !=
          core::RetainedProductKind::kPlanningCollisionModel) {
    fail_retained_bundle(
        "retained product references an unqualified contract");
  }
  if (product.artifact_refs.size() > kFlattenedModelArtifactRoles.size()) {
    fail_retained_bundle(
        "retained product contains more artifacts than its contract admits");
  }

  std::set<std::string> present_roles;
  std::string previous_role;
  for (const auto& artifact : product.artifact_refs) {
    require_retained_portable_identity(artifact.artifact_role_id.value(),
                                       "artifact_role_id");
    if (!previous_role.empty() &&
        previous_role >= artifact.artifact_role_id.value()) {
      fail_retained_bundle(
          "retained artifact roles must be strictly canonical-sorted");
    }
    previous_role = artifact.artifact_role_id.value();
    const auto expected = std::find_if(
        kFlattenedModelArtifactRoles.begin(),
        kFlattenedModelArtifactRoles.end(),
        [&artifact](const QualifiedRetainedProductArtifactRole& role) {
          return role.artifact_role_id == artifact.artifact_role_id.value();
        });
    if (expected == kFlattenedModelArtifactRoles.end()) {
      fail_retained_bundle(
          "retained product contains an artifact role outside its contract");
    }
    if (artifact.media_type != expected->media_type) {
      fail_retained_bundle(
          "retained artifact media type does not match its contract");
    }
    require_retained_digest(artifact.content_digest.value(),
                            "content_digest");
    if (artifact.content_addressed_store_key.value() !=
        retained_store_key_for_digest(artifact.content_digest.value())) {
      fail_retained_bundle(
          "retained artifact store key is not digest-derived");
    }
    if (artifact.byte_size > detail::kMaximumIJsonInteger) {
      fail_retained_bundle(
          "retained artifact byte size exceeds the I-JSON safe integer");
    }
    present_roles.insert(artifact.artifact_role_id.value());
  }
  for (const auto& expected : kFlattenedModelArtifactRoles) {
    if (expected.required &&
        !present_roles.contains(std::string(expected.artifact_role_id))) {
      fail_retained_bundle(
          "retained product is missing a required artifact role");
    }
  }
}

CanonicalValue
retained_product_projection(const core::RetainedProductDTO& product) {
  require_current_retained_product_conformance(product);
  CanonicalValue::Array artifacts;
  artifacts.reserve(product.artifact_refs.size());
  for (const auto& artifact : product.artifact_refs) {
    artifacts.push_back(retained_artifact_projection(artifact));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"product_id", CanonicalValue(product.product_id.value())},
      {"product_kind",
       CanonicalValue(std::string(core::to_string(product.product_kind)))},
      {"product_contract_id",
       CanonicalValue(product.product_contract_id.value())},
      {"product_contract_revision",
       CanonicalValue(product.product_contract_revision.value())},
      {"artifact_refs", CanonicalValue(std::move(artifacts))},
  });
}

core::ContentDigest
retained_product_output_digest(const core::RetainedProductDTO& product) {
  return core::ContentDigest(domain_separated_canonical_digest(
      kRetainedProductOutputDomain, kRetainedProductSchemaTag,
      retained_product_projection(product)));
}

std::shared_ptr<const RetainedBundle>
RetainedBundleConstructionAuthority::construct(
    core::RetainedBundleId retained_bundle_id,
    core::RetainedBundleDTO value, CanonicalValue normalized_root,
    std::string canonical_bytes) {
  return std::shared_ptr<const RetainedBundle>(new RetainedBundle(
      std::move(retained_bundle_id), std::move(value),
      std::move(normalized_root), std::move(canonical_bytes)));
}

}  // namespace detail

bool verify_retained_bundle_identity(const RetainedBundle& bundle) {
  try {
    const core::RetainedBundleDTO parsed =
        verify_current_retained_bundle_projection(bundle.normalized_root());
    const CanonicalValue* declared_id =
        bundle.normalized_root().find_object_member("retained_bundle_id");
    const auto* declared_id_string =
        declared_id == nullptr
            ? nullptr
            : std::get_if<std::string>(&declared_id->storage());
    if (parsed != bundle.value() ||
        bundle.value().retained_bundle_id != bundle.retained_bundle_id() ||
        declared_id_string == nullptr ||
        *declared_id_string != bundle.retained_bundle_id().value() ||
        bundle.canonical_bytes() != canonical_json(bundle.normalized_root()) ||
        !detail::verify_content_identity(
            bundle.normalized_root(),
            detail::ContentIdentitySpec{
                .identity_domain = "retained-bundle-id",
                .api_version = std::string(core::kRetainedBundleApiVersion),
                .own_field = "retained_bundle_id",
            })) {
      return false;
    }
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

std::shared_ptr<const RetainedBundle>
detail::RetainedBundleReloadAuthority::reload(
    std::string_view canonical_bytes) {
  try {
    const ParsedArtifactCandidate parsed =
        parse_strict_json_model_yaml(canonical_bytes);
    CanonicalValue root = parsed.root();
    if (canonical_json(root) != canonical_bytes) {
      fail_retained_bundle(
          "retained-bundle repository bytes are not RFC 8785 canonical");
    }
    core::RetainedBundleDTO value =
        verify_current_retained_bundle_projection(root);
    const core::RetainedBundleId retained_bundle_id =
        value.retained_bundle_id;
    auto bundle = RetainedBundleConstructionAuthority::construct(
        retained_bundle_id, std::move(value), std::move(root),
        std::string(canonical_bytes));
    if (!verify_retained_bundle_identity(*bundle)) {
      fail_retained_bundle(
          "retained-bundle repository bytes have an invalid semantic "
          "identity");
    }
    return bundle;
  } catch (const CanonicalValueError& error) {
    fail_retained_bundle(
        std::string("retained-bundle repository bytes are invalid: ") +
        error.what());
  } catch (const ArtifactCodecError& error) {
    fail_retained_bundle(
        std::string("retained-bundle repository bytes are invalid: ") +
        error.what());
  } catch (const CellGraphCodecError& error) {
    fail_retained_bundle(
        std::string("retained-bundle repository bytes are invalid: ") +
        error.what());
  }
}

}  // namespace manyforge::deployment
