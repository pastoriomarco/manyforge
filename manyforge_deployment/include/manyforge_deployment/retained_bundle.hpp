#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::deployment {

namespace detail {
class RetainedBundleMaterializer;
}

inline constexpr std::size_t kMaxInMemoryRetainedBlobCount = 4096U;
inline constexpr std::uint64_t kMaxInMemoryRetainedBlobBytes =
    512ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kMaxInMemoryRetainedBundleCount = 1024U;
inline constexpr std::uint64_t kMaxInMemoryRetainedBundleBytes =
    512ULL * 1024ULL * 1024ULL;

class RetainedBundleError : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

enum class ProductPlacement : std::uint8_t {
  kCellPlanningCollisionModel = 0,
  kCellViewerModel,
  kControlDomain,
  kSymbolRoleMap,
  kRuntimeUnit,
};

[[nodiscard]] constexpr std::string_view
to_string(ProductPlacement placement) noexcept {
  switch (placement) {
  case ProductPlacement::kCellPlanningCollisionModel:
    return "cell_planning_collision_model";
  case ProductPlacement::kCellViewerModel:
    return "cell_viewer_model";
  case ProductPlacement::kControlDomain:
    return "control_domain";
  case ProductPlacement::kSymbolRoleMap:
    return "symbol_role_map";
  case ProductPlacement::kRuntimeUnit:
    return "runtime_unit";
  }
  return {};
}

enum class ProductArtifactCardinality : std::uint8_t {
  kRequiredExactlyOne = 0,
  kOptionalAtMostOne,
};

[[nodiscard]] constexpr std::string_view
to_string(ProductArtifactCardinality cardinality) noexcept {
  switch (cardinality) {
  case ProductArtifactCardinality::kRequiredExactlyOne:
    return "required_exactly_one";
  case ProductArtifactCardinality::kOptionalAtMostOne:
    return "optional_at_most_one";
  }
  return {};
}

struct ProductContractArtifactRole final {
  core::ArtifactRoleId artifact_role_id;
  std::string media_type;
  ProductArtifactCardinality cardinality;

  friend bool operator==(const ProductContractArtifactRole &,
                         const ProductContractArtifactRole &) = default;
};

/// One exact descriptor qualified into the process build.
///
/// Callers cannot construct or add registry rows. The normalized descriptor
/// includes its declared revision and is retained for independent verification.
class BuildQualifiedProductContract final {
public:
  [[nodiscard]] const core::ProductContractId &
  product_contract_id() const noexcept {
    return product_contract_id_;
  }
  [[nodiscard]] const core::ProductContractRevision &
  product_contract_revision() const noexcept {
    return product_contract_revision_;
  }
  [[nodiscard]] core::RetainedProductKind product_kind() const noexcept {
    return product_kind_;
  }
  [[nodiscard]] ProductPlacement placement() const noexcept {
    return placement_;
  }
  [[nodiscard]] std::span<const ProductContractArtifactRole>
  artifact_roles() const noexcept {
    return artifact_roles_;
  }
  [[nodiscard]] const CanonicalValue &normalized_descriptor() const noexcept {
    return normalized_descriptor_;
  }

private:
  BuildQualifiedProductContract(
      core::ProductContractId product_contract_id,
      core::ProductContractRevision product_contract_revision,
      core::RetainedProductKind product_kind, ProductPlacement placement,
      std::vector<ProductContractArtifactRole> artifact_roles,
      CanonicalValue normalized_descriptor);

  core::ProductContractId product_contract_id_;
  core::ProductContractRevision product_contract_revision_;
  core::RetainedProductKind product_kind_;
  ProductPlacement placement_;
  std::vector<ProductContractArtifactRole> artifact_roles_;
  CanonicalValue normalized_descriptor_;

  friend class BuildQualifiedProductContractRegistry;
};

/// Immutable code-owned product-contract qualification registry.
class BuildQualifiedProductContractRegistry final {
public:
  [[nodiscard]] std::span<const BuildQualifiedProductContract>
  contracts() const noexcept {
    return contracts_;
  }
  [[nodiscard]] const BuildQualifiedProductContract *
  find(const core::ProductContractId &id,
       const core::ProductContractRevision &revision) const noexcept;

private:
  BuildQualifiedProductContractRegistry();

  std::vector<BuildQualifiedProductContract> contracts_;

  friend const BuildQualifiedProductContractRegistry &
  build_qualified_product_contract_registry();
};

[[nodiscard]] const BuildQualifiedProductContractRegistry &
build_qualified_product_contract_registry();

struct StoredRawArtifact final {
  core::ContentDigest content_digest;
  core::ContentAddressedStoreKey content_addressed_store_key;
  std::uint64_t byte_size;
  bool inserted;

  friend bool operator==(const StoredRawArtifact &,
                         const StoredRawArtifact &) = default;
};

/// Raw immutable-blob CAS port. It owns bytes, not semantic products or an
/// active bundle selection.
class IRawArtifactStore {
public:
  virtual ~IRawArtifactStore() = default;

  [[nodiscard]] virtual StoredRawArtifact
  put_if_absent(std::string_view bytes) = 0;
  [[nodiscard]] virtual std::optional<std::string>
  get_verified(const core::ContentAddressedStoreKey &key,
               const core::ContentDigest &expected_digest,
               std::uint64_t expected_size) const = 0;
};

class InMemoryRawArtifactStore final : public IRawArtifactStore {
public:
  explicit InMemoryRawArtifactStore(
      std::size_t max_blob_count = kMaxInMemoryRetainedBlobCount,
      std::uint64_t max_total_bytes = kMaxInMemoryRetainedBlobBytes,
      std::uint64_t max_single_blob_bytes = kMaxInMemoryRetainedBlobBytes);
  ~InMemoryRawArtifactStore() override;

  InMemoryRawArtifactStore(const InMemoryRawArtifactStore &) = delete;
  InMemoryRawArtifactStore &
  operator=(const InMemoryRawArtifactStore &) = delete;
  InMemoryRawArtifactStore(InMemoryRawArtifactStore &&) = delete;
  InMemoryRawArtifactStore &
  operator=(InMemoryRawArtifactStore &&) = delete;

  [[nodiscard]] StoredRawArtifact
  put_if_absent(std::string_view bytes) override;
  [[nodiscard]] std::optional<std::string>
  get_verified(const core::ContentAddressedStoreKey &key,
               const core::ContentDigest &expected_digest,
               std::uint64_t expected_size) const override;

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::uint64_t retained_bytes() const noexcept;

private:
  struct Storage;
  std::unique_ptr<Storage> storage_;
};

/// Immutable semantic retained-bundle value.
///
/// Construction remains private to the I3 materializer. That producer accepts
/// only RetentionQualifiedDeployment plus separately build-qualified offline
/// environment/provenance evidence; identity-only graphs and caller-built
/// canonical trees have no retention path.
class RetainedBundle final {
public:
  ~RetainedBundle() = default;
  RetainedBundle(const RetainedBundle &) = delete;
  RetainedBundle &operator=(const RetainedBundle &) = delete;
  RetainedBundle(RetainedBundle &&) = delete;
  RetainedBundle &operator=(RetainedBundle &&) = delete;

  [[nodiscard]] const core::RetainedBundleId &
  retained_bundle_id() const noexcept {
    return retained_bundle_id_;
  }
  [[nodiscard]] const core::RetainedBundleDTO& value() const noexcept {
    return value_;
  }
  [[nodiscard]] const CanonicalValue &normalized_root() const noexcept {
    return normalized_root_;
  }
  [[nodiscard]] std::string_view canonical_bytes() const noexcept {
    return canonical_bytes_;
  }

private:
  RetainedBundle(core::RetainedBundleId retained_bundle_id,
                 core::RetainedBundleDTO value,
                 CanonicalValue normalized_root, std::string canonical_bytes)
      : retained_bundle_id_(std::move(retained_bundle_id)),
        value_(std::move(value)),
        normalized_root_(std::move(normalized_root)),
        canonical_bytes_(std::move(canonical_bytes)) {}

  core::RetainedBundleId retained_bundle_id_;
  core::RetainedBundleDTO value_;
  CanonicalValue normalized_root_;
  std::string canonical_bytes_;

  friend class detail::RetainedBundleMaterializer;
};

[[nodiscard]] bool
verify_retained_bundle_identity(const RetainedBundle &bundle);

/// Semantic retained-bundle repository port. It owns immutable envelopes and
/// never owns an active/candidate alias or Apply selection.
class IRetainedBundleRepository {
public:
  virtual ~IRetainedBundleRepository() = default;

  [[nodiscard]] virtual bool
  put_if_absent(std::shared_ptr<const RetainedBundle> bundle) = 0;
  [[nodiscard]] virtual std::shared_ptr<const RetainedBundle>
  get_verified(const core::RetainedBundleId &retained_bundle_id) const = 0;
};

class InMemoryRetainedBundleRepository final
    : public IRetainedBundleRepository {
public:
  explicit InMemoryRetainedBundleRepository(
      std::size_t max_bundle_count = kMaxInMemoryRetainedBundleCount,
      std::uint64_t max_total_bytes = kMaxInMemoryRetainedBundleBytes);
  ~InMemoryRetainedBundleRepository() override;

  InMemoryRetainedBundleRepository(const InMemoryRetainedBundleRepository &) =
      delete;
  InMemoryRetainedBundleRepository &
  operator=(const InMemoryRetainedBundleRepository &) = delete;
  InMemoryRetainedBundleRepository(
      InMemoryRetainedBundleRepository &&) = delete;
  InMemoryRetainedBundleRepository &
  operator=(InMemoryRetainedBundleRepository &&) = delete;

  [[nodiscard]] bool
  put_if_absent(std::shared_ptr<const RetainedBundle> bundle) override;
  [[nodiscard]] std::shared_ptr<const RetainedBundle>
  get_verified(const core::RetainedBundleId &retained_bundle_id) const override;

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::uint64_t retained_bytes() const noexcept;

private:
  struct Storage;
  std::unique_ptr<Storage> storage_;
};

} // namespace manyforge::deployment
