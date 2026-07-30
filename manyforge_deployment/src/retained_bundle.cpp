#include "manyforge_deployment/retained_bundle.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "content_identity_internal.hpp"
#include "manyforge_deployment/content_identity.hpp"
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
constexpr std::uint64_t kMaximumIJsonInteger = 9007199254740991ULL;
constexpr std::size_t kMaximumPortableIdentityBytes = 128U;

[[noreturn]] void fail(std::string_view message) {
  throw RetainedBundleError(std::string(message));
}

[[nodiscard]] constexpr bool
is_ascii_alphanumeric(unsigned char byte) noexcept {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9');
}

[[nodiscard]] bool is_portable_identity(std::string_view value) noexcept {
  if (value.empty() || value.size() > kMaximumPortableIdentityBytes ||
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

[[nodiscard]] std::string store_key_for_digest(std::string_view digest) {
  require_digest(digest, "content_digest");
  return "sha256/" + std::string(digest.substr(7U));
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
    if (artifact.byte_size > kMaximumIJsonInteger) {
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
  std::vector<ProductContractArtifactRole> artifact_roles{
      {core::ArtifactRoleId("srdf"), "application/xml",
       ProductArtifactCardinality::kRequiredExactlyOne},
      {core::ArtifactRoleId("urdf"), "application/xml",
       ProductArtifactCardinality::kRequiredExactlyOne},
  };
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
        found->second->bytes.size() > kMaximumIJsonInteger) {
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

bool verify_retained_bundle_identity(const RetainedBundle &bundle) {
  try {
    const CanonicalValue *declared_id =
        bundle.normalized_root().find_object_member("retained_bundle_id");
    const auto *declared_id_string =
        declared_id == nullptr
            ? nullptr
            : std::get_if<std::string>(&declared_id->storage());
    if (declared_id_string == nullptr ||
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
