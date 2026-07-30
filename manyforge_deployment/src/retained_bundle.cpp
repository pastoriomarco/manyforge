#include "manyforge_deployment/retained_bundle.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "content_identity_internal.hpp"
#include "compiler_semantic_limits_internal.hpp"
#include "build_manifest_internal.hpp"
#include "deployment_catalog_codec_internal.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/cell_graph_codec.hpp"
#include "manyforge_deployment/closed_schema.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "manyforge_deployment/model_source.hpp"
#include "retained_bundle_internal.hpp"

namespace manyforge::deployment {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kFlattenedModelContractId =
    "manyforge.flattened_model";
constexpr std::string_view kContentIdentityPrefix =
    "manyforge.content-identity.v1";
constexpr std::string_view kRetainedProductOutputDomain =
    "retained-product-output";
constexpr std::string_view kRetainedProductSchemaTag =
    "manyforge.retained_bundle.v1#retained_product";
[[noreturn]] void fail(std::string_view message) {
  throw RetainedBundleError(std::string(message));
}

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

void require_portable_identity(std::string_view value, std::string_view field) {
  if (!is_portable_identity(value)) {
    fail(std::string(field) +
         " must use the portable 1..128-byte identity grammar");
  }
}

void require_digest(std::string_view value, std::string_view field) {
  if (!is_sha256_digest(value)) {
    fail(std::string(field) +
         " must be an exact lowercase sha256:<64 hex> digest");
  }
}

[[nodiscard]] const CanonicalValue::Object&
require_object(const CanonicalValue& value, std::string_view field) {
  const auto* object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail(std::string(field) + " must be an object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue::Array&
require_array(const CanonicalValue& value, std::string_view field) {
  const auto* array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail(std::string(field) + " must be an array");
  }
  return *array;
}

[[nodiscard]] const std::string&
require_string(const CanonicalValue& value, std::string_view field) {
  const auto* text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail(std::string(field) + " must be a string");
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
    fail(std::string(field) + " is missing required member " +
         std::string(name));
  }
  return found->second;
}

void require_exact_fields(
    const CanonicalValue::Object& object,
    std::initializer_list<std::string_view> expected,
    std::string_view field) {
  if (object.size() != expected.size()) {
    fail(std::string(field) + " has an incorrect closed field count");
  }
  for (const auto& [name, unused] : object) {
    (void)unused;
    if (std::find(expected.begin(), expected.end(), name) == expected.end()) {
      fail(std::string(field) + " contains unknown member " + name);
    }
  }
  for (const std::string_view name : expected) {
    (void)require_member(object, name, field);
  }
}

void require_empty_array(const CanonicalValue& value,
                         std::string_view field) {
  if (!require_array(value, field).empty()) {
    fail(std::string(field) +
         " is outside the currently qualified canonical-empty subset");
  }
}

void require_null(const CanonicalValue& value, std::string_view field) {
  if (!std::holds_alternative<std::nullptr_t>(value.storage())) {
    fail(std::string(field) + " must be null");
  }
}

[[nodiscard]] std::uint64_t require_i_json_size(
    const CanonicalValue& value, std::string_view field) {
  const auto* number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number) || *number < 0.0 ||
      std::trunc(*number) != *number ||
      *number >
          static_cast<double>(detail::kMaximumIJsonInteger)) {
    fail(std::string(field) +
         " must be a non-negative I-JSON safe integer");
  }
  return static_cast<std::uint64_t>(*number);
}

[[nodiscard]] std::string store_key_for_digest(std::string_view digest) {
  require_digest(digest, "content_digest");
  return "sha256/" + std::string(digest.substr(7U));
}

[[nodiscard]] const BuildQualifiedProductContract&
require_product_contract_conformance(
    const core::RetainedProductDTO& product,
    std::optional<ProductPlacement> expected_placement);

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
    fail("canonical-empty planning product has an invalid product_kind");
  }
  const std::string& contract_id = require_string(
      require_member(object, "product_contract_id", field),
      "compiled_products.planning_collision_model.product_contract_id");
  const std::string& contract_revision = require_string(
      require_member(object, "product_contract_revision", field),
      "compiled_products.planning_collision_model."
      "product_contract_revision");
  require_digest(contract_revision, "product_contract_revision");

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
  (void)require_product_contract_conformance(
      product, ProductPlacement::kCellPlanningCollisionModel);
  if (detail::retained_product_projection(product) != value) {
    fail("planning product differs from its exact typed projection");
  }
  return product;
}

[[nodiscard]] CanonicalValue
artifact_role_projection(const ProductContractArtifactRole &role) {
  return CanonicalValue(CanonicalValue::Object{
      {"artifact_role_id", CanonicalValue(role.artifact_role_id.value())},
      {"media_type", CanonicalValue(role.media_type)},
      {"cardinality", CanonicalValue(std::string(to_string(role.cardinality)))},
  });
}

[[nodiscard]] CanonicalValue product_contract_projection(
    std::string_view revision,
    std::span<const ProductContractArtifactRole> artifact_roles) {
  CanonicalValue::Array roles;
  roles.reserve(artifact_roles.size());
  for (const auto &role : artifact_roles) {
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
      {"placement", CanonicalValue(std::string(to_string(
                        ProductPlacement::kCellPlanningCollisionModel)))},
      {"artifact_roles", CanonicalValue(std::move(roles))},
  });
}

[[nodiscard]] std::string compute_product_contract_revision(
    std::span<const ProductContractArtifactRole> artifact_roles) {
  return detail::compute_content_identity(
      product_contract_projection(kZeroDigest, artifact_roles),
      detail::ContentIdentitySpec{
          .identity_domain = "product-contract-revision",
          .api_version = std::string(core::kProductContractApiVersion),
          .own_field = "product_contract_revision",
      });
}

[[nodiscard]] const std::vector<ProductContractArtifactRole>&
flattened_model_artifact_roles() {
  static const std::vector<ProductContractArtifactRole> roles{
      {core::ArtifactRoleId("srdf"), "application/xml",
       ProductArtifactCardinality::kRequiredExactlyOne},
      {core::ArtifactRoleId("urdf"), "application/xml",
       ProductArtifactCardinality::kRequiredExactlyOne},
  };
  return roles;
}

[[nodiscard]] const BuildQualifiedProductContract&
require_flattened_model_contract() {
  const auto& expected_roles = flattened_model_artifact_roles();
  const core::ProductContractRevision expected_revision(
      compute_product_contract_revision(expected_roles));
  const auto* contract = build_qualified_product_contract_registry().find(
      core::ProductContractId(std::string(kFlattenedModelContractId)),
      expected_revision);
  if (contract == nullptr ||
      contract->product_kind() !=
          core::RetainedProductKind::kPlanningCollisionModel ||
      contract->placement() !=
          ProductPlacement::kCellPlanningCollisionModel ||
      !std::equal(contract->artifact_roles().begin(),
                  contract->artifact_roles().end(), expected_roles.begin(),
                  expected_roles.end())) {
    throw std::logic_error(
        "build-qualified flattened-model product contract is absent or "
        "incoherent");
  }
  return *contract;
}

[[nodiscard]] CanonicalValue
retained_artifact_projection(const core::RetainedArtifactRefDTO &artifact) {
  return CanonicalValue(CanonicalValue::Object{
      {"artifact_role_id", CanonicalValue(artifact.artifact_role_id.value())},
      {"media_type", CanonicalValue(artifact.media_type)},
      {"byte_size", CanonicalValue(static_cast<double>(artifact.byte_size))},
      {"content_digest", CanonicalValue(artifact.content_digest.value())},
      {"content_addressed_store_key",
       CanonicalValue(artifact.content_addressed_store_key.value())},
  });
}

[[nodiscard]] std::string
domain_separated_canonical_digest(std::string_view domain,
                                  std::string_view schema_tag,
                                  const CanonicalValue &value) {
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

[[nodiscard]] const BuildQualifiedProductContract &
require_product_contract_conformance(
    const core::RetainedProductDTO &product,
    std::optional<ProductPlacement> expected_placement = std::nullopt) {
  require_portable_identity(product.product_id.value(), "product_id");
  const auto *contract = build_qualified_product_contract_registry().find(
      product.product_contract_id, product.product_contract_revision);
  if (contract == nullptr) {
    fail("retained product references an unqualified contract");
  }
  if (contract->product_kind() != product.product_kind ||
      (expected_placement.has_value() &&
       contract->placement() != *expected_placement)) {
    fail("retained product kind or placement does not match its contract");
  }
  if (product.artifact_refs.size() > contract->artifact_roles().size()) {
    fail("retained product contains more artifacts than its contract admits");
  }

  std::set<std::string> present_roles;
  std::string previous_role;
  for (const auto &artifact : product.artifact_refs) {
    require_portable_identity(artifact.artifact_role_id.value(),
                              "artifact_role_id");
    if (!previous_role.empty() &&
        previous_role >= artifact.artifact_role_id.value()) {
      fail("retained artifact roles must be strictly canonical-sorted");
    }
    previous_role = artifact.artifact_role_id.value();
    const auto expected = std::find_if(
        contract->artifact_roles().begin(), contract->artifact_roles().end(),
        [&artifact](const ProductContractArtifactRole &role) {
          return role.artifact_role_id == artifact.artifact_role_id;
        });
    if (expected == contract->artifact_roles().end()) {
      fail("retained product contains an artifact role outside its contract");
    }
    if (artifact.media_type != expected->media_type) {
      fail("retained artifact media type does not match its contract");
    }
    require_digest(artifact.content_digest.value(), "content_digest");
    if (artifact.content_addressed_store_key.value() !=
        store_key_for_digest(artifact.content_digest.value())) {
      fail("retained artifact store key is not digest-derived");
    }
    if (artifact.byte_size > detail::kMaximumIJsonInteger) {
      fail("retained artifact byte size exceeds the I-JSON safe integer");
    }
    present_roles.insert(artifact.artifact_role_id.value());
  }
  for (const auto &expected : contract->artifact_roles()) {
    const bool present =
        present_roles.contains(expected.artifact_role_id.value());
    if (expected.cardinality ==
            ProductArtifactCardinality::kRequiredExactlyOne &&
        !present) {
      fail("retained product is missing a required artifact role");
    }
  }
  return *contract;
}

} // namespace

BuildQualifiedProductContract::BuildQualifiedProductContract(
    core::ProductContractId product_contract_id,
    core::ProductContractRevision product_contract_revision,
    core::RetainedProductKind product_kind, ProductPlacement placement,
    std::vector<ProductContractArtifactRole> artifact_roles,
    CanonicalValue normalized_descriptor)
    : product_contract_id_(std::move(product_contract_id)),
      product_contract_revision_(std::move(product_contract_revision)),
      product_kind_(product_kind), placement_(placement),
      artifact_roles_(std::move(artifact_roles)),
      normalized_descriptor_(std::move(normalized_descriptor)) {}

BuildQualifiedProductContractRegistry::BuildQualifiedProductContractRegistry() {
  std::vector<ProductContractArtifactRole> artifact_roles(
      flattened_model_artifact_roles());
  const std::string revision =
      compute_product_contract_revision(artifact_roles);
  CanonicalValue descriptor =
      product_contract_projection(revision, artifact_roles);
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
  contracts_.push_back(BuildQualifiedProductContract(
      core::ProductContractId(std::string(kFlattenedModelContractId)),
      core::ProductContractRevision(revision),
      core::RetainedProductKind::kPlanningCollisionModel,
      ProductPlacement::kCellPlanningCollisionModel, std::move(artifact_roles),
      std::move(descriptor)));
}

const BuildQualifiedProductContract *
BuildQualifiedProductContractRegistry::find(
    const core::ProductContractId &id,
    const core::ProductContractRevision &revision) const noexcept {
  const auto match = std::find_if(
      contracts_.begin(), contracts_.end(),
      [&id, &revision](const BuildQualifiedProductContract &contract) {
        return contract.product_contract_id() == id &&
               contract.product_contract_revision() == revision;
      });
  return match == contracts_.end() ? nullptr : &*match;
}

const BuildQualifiedProductContractRegistry &
build_qualified_product_contract_registry() {
  static const BuildQualifiedProductContractRegistry registry;
  return registry;
}

struct InMemoryRawArtifactStore::Storage final {
  Storage(std::size_t max_blob_count_in, std::uint64_t max_total_bytes_in,
          std::uint64_t max_single_blob_bytes_in)
      : max_blob_count(max_blob_count_in), max_total_bytes(max_total_bytes_in),
        max_single_blob_bytes(max_single_blob_bytes_in) {}

  std::size_t max_blob_count;
  std::uint64_t max_total_bytes;
  std::uint64_t max_single_blob_bytes;
  std::map<std::string, std::string> blobs;
  std::uint64_t retained_bytes{0U};
  mutable std::mutex mutex;
};

InMemoryRawArtifactStore::InMemoryRawArtifactStore(
    std::size_t max_blob_count, std::uint64_t max_total_bytes,
    std::uint64_t max_single_blob_bytes) {
  if (max_blob_count == 0U || max_total_bytes == 0U ||
      max_single_blob_bytes == 0U || max_single_blob_bytes > max_total_bytes) {
    fail("raw artifact store limits must be positive and internally "
         "consistent");
  }
  storage_ = std::make_unique<Storage>(max_blob_count, max_total_bytes,
                                       max_single_blob_bytes);
}

InMemoryRawArtifactStore::~InMemoryRawArtifactStore() = default;

StoredRawArtifact
InMemoryRawArtifactStore::put_if_absent(std::string_view bytes) {
  if (bytes.size() > static_cast<std::size_t>(std::min<std::uint64_t>(
                         storage_->max_single_blob_bytes,
                         static_cast<std::uint64_t>(
                             std::numeric_limits<std::size_t>::max())))) {
    fail("raw artifact exceeds the configured single-blob bound");
  }
  const std::uint64_t byte_size = static_cast<std::uint64_t>(bytes.size());
  const std::string digest = sha256_digest(bytes);
  const std::string key = store_key_for_digest(digest);

  std::lock_guard lock(storage_->mutex);
  const auto existing = storage_->blobs.find(key);
  if (existing != storage_->blobs.end()) {
    if (existing->second.size() != bytes.size() ||
        !std::equal(existing->second.begin(), existing->second.end(),
                    bytes.begin(), bytes.end())) {
      throw std::runtime_error(
          "immutable raw artifact store detected a digest-key collision");
    }
    return {
        core::ContentDigest(digest),
        core::ContentAddressedStoreKey(key),
        byte_size,
        false,
    };
  }

  if (storage_->blobs.size() >= storage_->max_blob_count) {
    fail("raw artifact store entry bound is exhausted");
  }
  if (byte_size > storage_->max_total_bytes - storage_->retained_bytes) {
    fail("raw artifact store byte bound is exhausted");
  }
  storage_->blobs.emplace(key, std::string(bytes));
  storage_->retained_bytes += byte_size;
  return {
      core::ContentDigest(digest),
      core::ContentAddressedStoreKey(key),
      byte_size,
      true,
  };
}

std::optional<std::string> InMemoryRawArtifactStore::get_verified(
    const core::ContentAddressedStoreKey &key,
    const core::ContentDigest &expected_digest,
    std::uint64_t expected_size) const {
  if (!is_sha256_digest(expected_digest.value()) ||
      key.value() != store_key_for_digest(expected_digest.value())) {
    return std::nullopt;
  }
  std::string bytes;
  {
    std::lock_guard lock(storage_->mutex);
    const auto found = storage_->blobs.find(key.value());
    if (found == storage_->blobs.end()) {
      return std::nullopt;
    }
    bytes = found->second;
  }
  if (bytes.size() != expected_size ||
      sha256_digest(bytes) != expected_digest.value()) {
    return std::nullopt;
  }
  return bytes;
}

std::size_t InMemoryRawArtifactStore::size() const noexcept {
  std::lock_guard lock(storage_->mutex);
  return storage_->blobs.size();
}

std::uint64_t InMemoryRawArtifactStore::retained_bytes() const noexcept {
  std::lock_guard lock(storage_->mutex);
  return storage_->retained_bytes;
}

namespace detail {

core::RetainedProductDTO build_and_retain_product(
    const core::ProductId &product_id, core::RetainedProductKind product_kind,
    ProductPlacement placement,
    const core::ProductContractId &product_contract_id,
    const core::ProductContractRevision &product_contract_revision,
    std::span<const RetainedArtifactInput> artifacts,
    IRawArtifactStore &artifact_store) {
  require_portable_identity(product_id.value(), "product_id");
  const auto *contract = build_qualified_product_contract_registry().find(
      product_contract_id, product_contract_revision);
  if (contract == nullptr) {
    fail("product selects an unknown or unqualified contract revision");
  }
  if (contract->product_kind() != product_kind ||
      contract->placement() != placement) {
    fail("product kind or placement does not match its qualified contract");
  }
  if (artifacts.size() > contract->artifact_roles().size()) {
    fail("product contains more artifacts than its contract admits");
  }

  std::map<std::string, const RetainedArtifactInput *> supplied;
  for (const auto &artifact : artifacts) {
    require_portable_identity(artifact.artifact_role_id.value(),
                              "artifact_role_id");
    if (!supplied.emplace(artifact.artifact_role_id.value(), &artifact)
             .second) {
      fail("product contains a duplicate artifact role");
    }
  }

  for (const auto &[role, unused] : supplied) {
    (void)unused;
    const auto expected = std::find_if(
        contract->artifact_roles().begin(), contract->artifact_roles().end(),
        [&role](const ProductContractArtifactRole &candidate) {
          return candidate.artifact_role_id.value() == role;
        });
    if (expected == contract->artifact_roles().end()) {
      fail("product contains an artifact role not admitted by its contract");
    }
  }
  for (const auto &expected : contract->artifact_roles()) {
    const auto found = supplied.find(expected.artifact_role_id.value());
    if (expected.cardinality ==
            ProductArtifactCardinality::kRequiredExactlyOne &&
        found == supplied.end()) {
      fail("product is missing a required artifact role");
    }
    if (found != supplied.end() &&
        found->second->media_type != expected.media_type) {
      fail("product artifact media type does not match its contract");
    }
    if (found != supplied.end() &&
        found->second->bytes.size() >
            detail::kMaximumIJsonInteger) {
      fail("product artifact byte size exceeds the I-JSON safe integer");
    }
  }

  std::vector<core::RetainedArtifactRefDTO> retained;
  retained.reserve(supplied.size());
  for (const auto &expected : contract->artifact_roles()) {
    const auto found = supplied.find(expected.artifact_role_id.value());
    if (found == supplied.end()) {
      continue;
    }
    const std::uint64_t expected_byte_size =
        static_cast<std::uint64_t>(found->second->bytes.size());
    const std::string expected_digest = sha256_digest(found->second->bytes);
    const std::string expected_key = store_key_for_digest(expected_digest);
    const StoredRawArtifact stored =
        artifact_store.put_if_absent(found->second->bytes);
    if (stored.byte_size != expected_byte_size ||
        stored.content_digest.value() != expected_digest ||
        stored.content_addressed_store_key.value() != expected_key) {
      throw std::runtime_error(
          "raw artifact store returned inconsistent immutable metadata");
    }
    const auto read_back = artifact_store.get_verified(
        core::ContentAddressedStoreKey(expected_key),
        core::ContentDigest(expected_digest), expected_byte_size);
    if (!read_back.has_value() || *read_back != found->second->bytes) {
      throw std::runtime_error(
          "raw artifact store failed immutable put verification");
    }
    retained.push_back(core::RetainedArtifactRefDTO{
        expected.artifact_role_id,
        expected.media_type,
        expected_byte_size,
        core::ContentDigest(expected_digest),
        core::ContentAddressedStoreKey(expected_key),
    });
  }

  core::RetainedProductDTO product{
      product_id,          product_kind,
      product_contract_id, product_contract_revision,
      std::move(retained),
  };
  (void)require_product_contract_conformance(product, placement);
  return product;
}

CanonicalValue
retained_product_projection(const core::RetainedProductDTO &product) {
  (void)require_product_contract_conformance(product);
  CanonicalValue::Array artifacts;
  artifacts.reserve(product.artifact_refs.size());
  for (const auto &artifact : product.artifact_refs) {
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
retained_product_output_digest(const core::RetainedProductDTO &product) {
  return core::ContentDigest(domain_separated_canonical_digest(
      kRetainedProductOutputDomain, kRetainedProductSchemaTag,
      retained_product_projection(product)));
}

}  // namespace detail

namespace {

[[nodiscard]] CanonicalValue node_environment_projection(
    const core::RetainedNodeEnvironmentDTO& environment) {
  return CanonicalValue(CanonicalValue::Object{
      {"catalog_id", CanonicalValue(environment.catalog_id)},
      {"content_version", CanonicalValue(environment.content_version)},
      {"catalog_digest",
       CanonicalValue(environment.catalog_digest.value())},
      {"environment_digest",
       CanonicalValue(environment.environment_digest.value())},
  });
}

[[nodiscard]] CanonicalValue records_projection(
    std::span<const core::CatalogRecordRefDTO> records) {
  const CanonicalValue catalogs =
      detail::encode_deployment_catalogs(records);
  const CanonicalValue* encoded_records =
      catalogs.find_object_member("records");
  if (encoded_records == nullptr) {
    throw std::logic_error(
        "deployment catalog codec omitted its required records member");
  }
  return *encoded_records;
}

[[nodiscard]] CanonicalValue catalogs_projection(
    std::span<const core::CatalogRecordRefDTO> records,
    const core::RetainedNodeEnvironmentDTO& environment) {
  const auto& schema_registry = build_qualified_closed_schema_registry();
  return CanonicalValue(CanonicalValue::Object{
      {"dialect_api_version",
       CanonicalValue(std::string(kClosedSchemaApiVersion))},
      {"bootstrap_contract_digest",
       CanonicalValue(std::string(kClosedSchemaBootstrapContractDigest))},
      {"qualification_registry_digest",
       CanonicalValue(
           schema_registry.qualification_registry_digest().value())},
      {"records", records_projection(records)},
      // No retained catalog record means no selected schema or transitive
      // ValueSchemaRef, so the exact closure is the empty table.
      {"schema_artifacts", CanonicalValue(CanonicalValue::Array{})},
      {"node_environment", node_environment_projection(environment)},
  });
}

[[nodiscard]] CanonicalValue compiled_products_projection(
    const core::RetainedProductDTO& planning_collision_model) {
  return CanonicalValue(CanonicalValue::Object{
      {"planning_collision_model",
       detail::retained_product_projection(planning_collision_model)},
      {"viewer_model", CanonicalValue(nullptr)},
      {"control_domain_products", CanonicalValue(CanonicalValue::Array{})},
      {"symbol_role_maps", CanonicalValue(CanonicalValue::Array{})},
      {"runtime_unit_products", CanonicalValue(CanonicalValue::Array{})},
  });
}

[[nodiscard]] CanonicalValue provenance_projection(
    const detail::BuildQualifiedRetentionContext& context,
    const core::RetainedProductDTO& planning_collision_model) {
  const core::ContentDigest output_digest =
      detail::retained_product_output_digest(planning_collision_model);
  CanonicalValue::Array outputs;
  outputs.emplace_back(CanonicalValue::Object{
      {"product_id",
       CanonicalValue(planning_collision_model.product_id.value())},
      {"product_kind",
       CanonicalValue(std::string(
           core::to_string(planning_collision_model.product_kind)))},
      {"content_digest", CanonicalValue(output_digest.value())},
  });
  return CanonicalValue(CanonicalValue::Object{
      {"compiler_contract_digest",
       CanonicalValue(context.compiler_contract_digest().value())},
      {"compiler_build_digest",
       CanonicalValue(context.compiler_build_digest().value())},
      {"source_records", CanonicalValue(CanonicalValue::Array{})},
      {"generator_records", CanonicalValue(CanonicalValue::Array{})},
      {"output_records", CanonicalValue(std::move(outputs))},
  });
}

[[nodiscard]] core::RetainedBundleDTO retained_bundle_value(
    std::string_view retained_bundle_id,
    const RetentionQualifiedDeployment& qualified,
    const detail::BuildQualifiedRetentionContext& context,
    const core::RetainedProductDTO& planning_collision_model) {
  const auto& deployment = qualified.deployment();
  const auto& registry = build_qualified_closed_schema_registry();
  return core::RetainedBundleDTO{
      std::string(core::kRetainedBundleApiVersion),
      core::RetainedBundleId(std::string(retained_bundle_id)),
      deployment.deployment_artifact_id(),
      deployment.deployment_revision(),
      deployment.graph().graph(),
      core::RetainedCatalogDTO{
          std::string(kClosedSchemaApiVersion),
          core::ContentDigest(
              std::string(kClosedSchemaBootstrapContractDigest)),
          registry.qualification_registry_digest(),
          deployment.catalog_records(),
          {},
          context.node_environment(),
      },
      deployment.applicability_inputs()
          .value()
          .desired_control_configurations,
      deployment.applicability_inputs()
          .value()
          .capability_provider_bindings,
      core::RetainedCompiledProductsDTO{
          planning_collision_model,
          std::nullopt,
          {},
          {},
          {},
      },
      core::RetainedBundleProvenanceDTO{
          context.compiler_contract_digest(),
          context.compiler_build_digest(),
          {},
          {},
          {core::RetainedOutputRecordDTO{
              planning_collision_model.product_id,
              planning_collision_model.product_kind,
              detail::retained_product_output_digest(
                  planning_collision_model),
          }},
      },
  };
}

[[nodiscard]] CanonicalValue retained_bundle_projection(
    std::string_view retained_bundle_id,
    const RetentionQualifiedDeployment& qualified,
    const detail::BuildQualifiedRetentionContext& context,
    const core::RetainedProductDTO& planning_collision_model) {
  const auto& deployment = qualified.deployment();
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(core::kRetainedBundleApiVersion))},
      {"retained_bundle_id",
       CanonicalValue(std::string(retained_bundle_id))},
      {"deployment_artifact_id",
       CanonicalValue(deployment.deployment_artifact_id().value())},
      {"deployment_revision",
       CanonicalValue(deployment.deployment_revision().value())},
      {"cell_graph", deployment.graph().normalized_root()},
      {"catalogs",
       catalogs_projection(deployment.catalog_records(),
                           context.node_environment())},
      {"desired_control_configurations",
       deployment.applicability_inputs()
           .normalized_desired_control_configurations()},
      {"capability_provider_bindings",
       deployment.applicability_inputs()
           .normalized_capability_provider_bindings()},
      {"compiled_products",
       compiled_products_projection(planning_collision_model)},
      {"provenance",
       provenance_projection(context, planning_collision_model)},
  });
}

void require_current_subset_before_store(
    const RetentionQualifiedDeployment& qualified) {
  const auto& deployment = qualified.deployment();
  const auto& graph = deployment.graph().graph();
  const auto& inputs = deployment.applicability_inputs().value();
  const auto& compiler = qualified.compiler_ir();

  if (!graph.runtime_units.empty() || !graph.model_instances.empty() ||
      !graph.controllable_components.empty() ||
      !graph.control_domains.empty() || !graph.control_bindings.empty() ||
      !graph.component_attachments.empty() ||
      !deployment.catalog_records().empty() ||
      !inputs.desired_control_configurations.empty() ||
      !inputs.capability_provider_bindings.empty() ||
      !inputs.external_resources.empty() || inputs.site_overlay_ref.has_value() ||
      !compiler.catalog_record_refs().empty()) {
    fail("retained-bundle materialization currently qualifies only the "
         "canonical-empty deployment subset");
  }

  const auto& applicability = compiler.applicability();
  const auto& composition = applicability.composition();
  if (!composition.model_assets().empty() ||
      !composition.component_types().empty() ||
      !composition.model_occurrences().empty() ||
      !composition.symbol_rewrites().empty() ||
      !composition.reverse_symbol_rewrites().empty() ||
      !composition.reference_rewrites().empty() ||
      !composition.frames().empty() ||
      !composition.fixed_connections().empty() ||
      !composition.coordinates().empty() ||
      !composition.control_binding_coordinate_maps().empty() ||
      !composition.targets().empty() ||
      !applicability.runtime_profiles().empty() ||
      !applicability.capability_definitions().empty() ||
      !applicability.control_interface_contracts().empty() ||
      !applicability.controller_topologies().empty() ||
      !applicability.provider_manifests().empty() ||
      !applicability.stop_evidence_policies().empty() ||
      !applicability.binding_interfaces().empty() ||
      !applicability.controllers().empty() ||
      !applicability.routes().empty() ||
      !applicability.capabilities().empty() ||
      !applicability.provider_endpoints().empty() ||
      !applicability.external_resources().empty() ||
      !applicability.stop_applicability().empty()) {
    fail("canonical-empty compiler IR contains an unexpected semantic "
         "product input");
  }

  // "Empty" means no authored model/component occurrence. The ordinary I2
  // compiler still emits the one semantic cell frame needed by every
  // projection, including the bootstrap projection. Keep that exact ordinary
  // product instead of defining an I3-only empty URDF shortcut.
  const auto& cell_frame = composition.cell_frame();
  const auto& urdf = composition.flattened_urdf();
  const auto& srdf = composition.flattened_srdf();
  const auto& frame_paths = composition.frame_paths();
  const auto* path_cell_frame =
      frame_paths.size() == 1U
          ? std::get_if<CompositionCellFrame>(
                &frame_paths.front().terminal_frame)
          : nullptr;
  if (cell_frame.cell_id != graph.cell_id ||
      urdf.robot_name.empty() || urdf.materials.size() != 0U ||
      urdf.links.size() != 1U || !urdf.joints.empty() ||
      urdf.links.front().name != cell_frame.compiled_frame_name ||
      urdf.links.front().inertial.has_value() ||
      !urdf.links.front().visuals.empty() ||
      !urdf.links.front().collisions.empty() ||
      srdf.robot_name != urdf.robot_name || !srdf.groups.empty() ||
      !srdf.group_states.empty() || !srdf.end_effectors.empty() ||
      !srdf.passive_joint_names.empty() ||
      !srdf.disabled_collisions.empty() || path_cell_frame == nullptr ||
      *path_cell_frame != cell_frame ||
      !frame_paths.front().segments.empty()) {
    fail("canonical-empty compiler IR does not contain the exact semantic "
         "cell-frame product");
  }
}

struct PreparedCanonicalEmptyProduct final {
  std::string urdf;
  std::string srdf;
  const BuildQualifiedProductContract* contract;
};

[[nodiscard]] PreparedCanonicalEmptyProduct
prepare_canonical_empty_product_before_store(
    const RetentionQualifiedDeployment& qualified) {
  require_current_subset_before_store(qualified);
  const auto& composition =
      qualified.compiler_ir().applicability().composition();
  std::string urdf =
      emit_canonical_urdf_document(composition.flattened_urdf());
  std::string srdf =
      emit_canonical_srdf_document(composition.flattened_srdf());
  const auto& contract = require_flattened_model_contract();
  return {std::move(urdf), std::move(srdf), &contract};
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
    fail("retained cell_graph differs from its exact verified projection");
  }
  const auto& graph = verified.graph();
  if (!graph.runtime_units.empty() || !graph.model_instances.empty() ||
      !graph.controllable_components.empty() ||
      !graph.control_domains.empty() || !graph.control_bindings.empty() ||
      !graph.component_attachments.empty()) {
    fail("retained cell_graph is outside the canonical-empty subset");
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
  require_portable_identity(
      require_string(require_member(object, "catalog_id",
                                    "catalogs.node_environment"),
                     "catalogs.node_environment.catalog_id"),
      "catalogs.node_environment.catalog_id");
  require_portable_identity(
      require_string(require_member(object, "content_version",
                                    "catalogs.node_environment"),
                     "catalogs.node_environment.content_version"),
      "catalogs.node_environment.content_version");
  require_digest(
      require_string(require_member(object, "catalog_digest",
                                    "catalogs.node_environment"),
                     "catalogs.node_environment.catalog_digest"),
      "catalogs.node_environment.catalog_digest");
  require_digest(
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
    fail("retained_bundle.api_version is not the exact v1 API");
  }
  const std::string& retained_bundle_id = require_string(
      require_member(object, "retained_bundle_id", "retained_bundle"),
      "retained_bundle.retained_bundle_id");
  require_digest(retained_bundle_id, "retained_bundle.retained_bundle_id");
  const std::string& deployment_artifact_id = require_string(
      require_member(object, "deployment_artifact_id", "retained_bundle"),
      "retained_bundle.deployment_artifact_id");
  require_portable_identity(
      deployment_artifact_id,
      "retained_bundle.deployment_artifact_id");
  const std::string& deployment_revision = require_string(
      require_member(object, "deployment_revision", "retained_bundle"),
      "retained_bundle.deployment_revision");
  require_digest(deployment_revision, "retained_bundle.deployment_revision");
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
    fail("retained catalog dialect does not match this build");
  }
  const std::string& bootstrap_contract_digest = require_string(
      require_member(catalogs, "bootstrap_contract_digest",
                     "retained_bundle.catalogs"),
      "retained_bundle.catalogs.bootstrap_contract_digest");
  if (bootstrap_contract_digest != kClosedSchemaBootstrapContractDigest) {
    fail("retained bootstrap contract does not match this build");
  }
  const std::string& qualification_registry_digest = require_string(
      require_member(catalogs, "qualification_registry_digest",
                     "retained_bundle.catalogs"),
      "retained_bundle.catalogs.qualification_registry_digest");
  if (qualification_registry_digest !=
      build_qualified_closed_schema_registry()
          .qualification_registry_digest()
          .value()) {
    fail("retained qualification registry does not match this build");
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
  require_digest(compiler_contract_digest,
                 "retained_bundle.provenance.compiler_contract_digest");
  const std::string& compiler_build_digest = require_string(
      require_member(provenance, "compiler_build_digest",
                     "retained_bundle.provenance"),
      "retained_bundle.provenance.compiler_build_digest");
  require_digest(compiler_build_digest,
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
    fail("canonical-empty provenance must contain exactly one output record");
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
    fail("retained output record does not match the complete planning "
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

detail::BuildQualifiedRetentionContext::BuildQualifiedRetentionContext(
    core::RetainedNodeEnvironmentDTO node_environment,
    core::ContentDigest compiler_contract_digest,
    core::ContentDigest compiler_build_digest)
    : node_environment_(std::move(node_environment)),
      compiler_contract_digest_(std::move(compiler_contract_digest)),
      compiler_build_digest_(std::move(compiler_build_digest)) {
  require_portable_identity(node_environment_.catalog_id,
                            "node_environment.catalog_id");
  require_portable_identity(node_environment_.content_version,
                            "node_environment.content_version");
  require_digest(node_environment_.catalog_digest.value(),
                 "node_environment.catalog_digest");
  require_digest(node_environment_.environment_digest.value(),
                 "node_environment.environment_digest");
  require_digest(compiler_contract_digest_.value(),
                 "compiler_contract_digest");
  require_digest(compiler_build_digest_.value(), "compiler_build_digest");
}

detail::BuildQualifiedRetentionContext::BuildQualifiedRetentionContext(
    core::RetainedNodeEnvironmentDTO node_environment,
    core::ContentDigest compiler_contract_digest,
    core::ContentDigest compiler_build_digest,
    std::vector<std::shared_ptr<const BuildQualifiedManifest>> manifests)
    : BuildQualifiedRetentionContext(
          std::move(node_environment), std::move(compiler_contract_digest),
          std::move(compiler_build_digest)) {
  if (manifests.size() != 3U || manifests[0] == nullptr ||
      manifests[1] == nullptr || manifests[2] == nullptr ||
      manifests[0]->key().manifest_kind !=
          BuildManifestKind::kNodeEnvironment ||
      manifests[1]->key().manifest_kind !=
          BuildManifestKind::kCompilerContract ||
      manifests[2]->key().manifest_kind !=
          BuildManifestKind::kCompilerBuild ||
      manifests[0]->key().content_digest !=
          node_environment_.environment_digest ||
      manifests[1]->key().content_digest != compiler_contract_digest_ ||
      manifests[2]->key().content_digest != compiler_build_digest_) {
    throw std::logic_error(
        "production retention context manifest closure is incoherent");
  }
  manifests_ = std::move(manifests);
}

std::shared_ptr<const RetainedBundle>
detail::RetainedBundleMaterializer::materialize_prepared(
    const RetentionQualifiedDeployment& qualified,
    const BuildQualifiedRetentionContext& context,
    std::string_view urdf, std::string_view srdf,
    const BuildQualifiedProductContract& contract,
    IRawArtifactStore& artifact_store) {
  const std::vector<detail::RetainedArtifactInput> artifacts{
      {core::ArtifactRoleId("srdf"), "application/xml",
       std::string(srdf)},
      {core::ArtifactRoleId("urdf"), "application/xml",
       std::string(urdf)},
  };
  const core::RetainedProductDTO planning =
      detail::build_and_retain_product(
          core::ProductId("cell_planning_collision_model"),
          core::RetainedProductKind::kPlanningCollisionModel,
          ProductPlacement::kCellPlanningCollisionModel,
          contract.product_contract_id(),
          contract.product_contract_revision(), artifacts,
          artifact_store);

  const CanonicalValue placeholder = retained_bundle_projection(
      kZeroDigest, qualified, context, planning);
  const std::string retained_bundle_id = detail::compute_content_identity(
      placeholder,
      detail::ContentIdentitySpec{
          .identity_domain = "retained-bundle-id",
          .api_version = std::string(core::kRetainedBundleApiVersion),
          .own_field = "retained_bundle_id",
      });
  CanonicalValue root = retained_bundle_projection(
      retained_bundle_id, qualified, context, planning);
  std::string bytes = canonical_json(root);
  core::RetainedBundleDTO value = retained_bundle_value(
      retained_bundle_id, qualified, context, planning);
  auto bundle = std::shared_ptr<const RetainedBundle>(new RetainedBundle(
      core::RetainedBundleId(retained_bundle_id), std::move(value),
      std::move(root), std::move(bytes)));
  if (!verify_retained_bundle_identity(*bundle)) {
    throw std::logic_error(
        "I3 materializer produced an invalid retained-bundle projection");
  }
  return bundle;
}

std::shared_ptr<const RetainedBundle>
detail::RetainedBundleMaterializer::materialize_production(
    const RetentionQualifiedDeployment& qualified,
    IBuildManifestRepository& manifest_repository,
    IRawArtifactStore& artifact_store) {
  // Complete semantic/subset preparation and build-authority qualification
  // precede the first repository or CAS effect.
  const PreparedCanonicalEmptyProduct prepared =
      prepare_canonical_empty_product_before_store(qualified);
  const BuildQualifiedRetentionContext context =
      BuildQualifiedRetentionAuthority::qualify_persisted(
          manifest_repository);
  return materialize_prepared(
      qualified, context, prepared.urdf, prepared.srdf,
      *prepared.contract, artifact_store);
}

std::shared_ptr<const RetainedBundle>
detail::RetainedBundleMaterializer::materialize(
    const RetentionQualifiedDeployment& qualified,
    const BuildQualifiedRetentionContext& context,
    IRawArtifactStore& artifact_store) {
  const PreparedCanonicalEmptyProduct prepared =
      prepare_canonical_empty_product_before_store(qualified);
  return materialize_prepared(
      qualified, context, prepared.urdf, prepared.srdf,
      *prepared.contract, artifact_store);
}

bool verify_retained_bundle_identity(const RetainedBundle &bundle) {
  try {
    const core::RetainedBundleDTO parsed =
        verify_current_retained_bundle_projection(bundle.normalized_root());
    const CanonicalValue *declared_id =
        bundle.normalized_root().find_object_member("retained_bundle_id");
    const auto *declared_id_string =
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
  } catch (const std::exception &) {
    return false;
  }
}

struct InMemoryRetainedBundleRepository::Storage final {
  struct Entry final {
    std::shared_ptr<const RetainedBundle> bundle;
    std::string exact_bytes;
  };

  Storage(std::size_t max_bundle_count_in, std::uint64_t max_total_bytes_in)
      : max_bundle_count(max_bundle_count_in),
        max_total_bytes(max_total_bytes_in) {}

  std::size_t max_bundle_count;
  std::uint64_t max_total_bytes;
  std::map<std::string, Entry> bundles;
  std::uint64_t retained_bytes{0U};
  mutable std::mutex mutex;
};

InMemoryRetainedBundleRepository::InMemoryRetainedBundleRepository(
    std::size_t max_bundle_count, std::uint64_t max_total_bytes) {
  if (max_bundle_count == 0U || max_total_bytes == 0U) {
    fail("retained bundle repository limits must be positive");
  }
  storage_ = std::make_unique<Storage>(max_bundle_count, max_total_bytes);
}

InMemoryRetainedBundleRepository::~InMemoryRetainedBundleRepository() = default;

bool InMemoryRetainedBundleRepository::put_if_absent(
    std::shared_ptr<const RetainedBundle> bundle) {
  if (bundle == nullptr) {
    fail("retained bundle repository rejects a null bundle");
  }
  if (!verify_retained_bundle_identity(*bundle)) {
    fail("retained bundle repository rejects an invalid semantic identity");
  }
  const std::uint64_t byte_size =
      static_cast<std::uint64_t>(bundle->canonical_bytes().size());
  std::lock_guard lock(storage_->mutex);
  const auto existing =
      storage_->bundles.find(bundle->retained_bundle_id().value());
  if (existing != storage_->bundles.end()) {
    if (existing->second.exact_bytes != bundle->canonical_bytes()) {
      throw std::runtime_error(
          "retained bundle repository detected an identity collision");
    }
    return false;
  }
  if (storage_->bundles.size() >= storage_->max_bundle_count) {
    fail("retained bundle repository entry bound is exhausted");
  }
  if (byte_size > storage_->max_total_bytes - storage_->retained_bytes) {
    fail("retained bundle repository byte bound is exhausted");
  }
  storage_->bundles.emplace(
      bundle->retained_bundle_id().value(),
      Storage::Entry{bundle, std::string(bundle->canonical_bytes())});
  storage_->retained_bytes += byte_size;
  return true;
}

std::shared_ptr<const RetainedBundle>
InMemoryRetainedBundleRepository::get_verified(
    const core::RetainedBundleId &retained_bundle_id) const {
  std::shared_ptr<const RetainedBundle> bundle;
  std::string exact_bytes;
  {
    std::lock_guard lock(storage_->mutex);
    const auto found = storage_->bundles.find(retained_bundle_id.value());
    if (found == storage_->bundles.end()) {
      return nullptr;
    }
    bundle = found->second.bundle;
    exact_bytes = found->second.exact_bytes;
  }
  if (bundle == nullptr || bundle->canonical_bytes() != exact_bytes ||
      !verify_retained_bundle_identity(*bundle)) {
    throw std::runtime_error(
        "retained bundle repository re-verification failed");
  }
  return bundle;
}

std::size_t InMemoryRetainedBundleRepository::size() const noexcept {
  std::lock_guard lock(storage_->mutex);
  return storage_->bundles.size();
}

std::uint64_t
InMemoryRetainedBundleRepository::retained_bytes() const noexcept {
  std::lock_guard lock(storage_->mutex);
  return storage_->retained_bytes;
}

} // namespace manyforge::deployment
