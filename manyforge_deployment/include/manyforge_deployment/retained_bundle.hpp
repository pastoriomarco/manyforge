#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
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
class RetainedBundleReloadAuthority;
}

class FilesystemVerifiedRetainedBundleSource;

inline constexpr std::size_t kMaxDurableRetainedBlobCount = 4096U;
inline constexpr std::uint64_t kMaxDurableRetainedBlobBytes =
    512ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kMaxDurableRetainedBundleCount = 1024U;
inline constexpr std::uint64_t kMaxDurableRetainedBundleBytes =
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

/// Durable immutable raw-artifact CAS.
///
/// Blobs are published under their digest-derived key only after a
/// same-directory write and file fsync. Publication uses a no-replace hard
/// link followed by directory fsync; committed names are never opened for
/// writing. Every reopen enumerates the complete bounded store and verifies
/// each filename, size, and digest before use.
///
/// `trusted_root` must be an existing deployment-owned plain directory. The
/// root and its ancestors must not be renamed, replaced, or modified by an
/// untrusted actor while the store is open. The implementation rejects
/// symlinks at every path it owns and serializes cooperating processes, but
/// it does not defend against a privileged writer that can replace the
/// trusted hierarchy or deliberately forge filesystem state. Store instances
/// are process-affine: after `fork()`, the child must construct a fresh store
/// before use so it owns a distinct advisory-lock file description.
class FilesystemRawArtifactStore final : public IRawArtifactStore {
public:
  explicit FilesystemRawArtifactStore(
      std::filesystem::path trusted_root,
      std::size_t max_blob_count = kMaxDurableRetainedBlobCount,
      std::uint64_t max_total_bytes = kMaxDurableRetainedBlobBytes,
      std::uint64_t max_single_blob_bytes =
          kMaxDurableRetainedBlobBytes);
  ~FilesystemRawArtifactStore() override;

  FilesystemRawArtifactStore(const FilesystemRawArtifactStore &) = delete;
  FilesystemRawArtifactStore &
  operator=(const FilesystemRawArtifactStore &) = delete;
  FilesystemRawArtifactStore(FilesystemRawArtifactStore &&) = delete;
  FilesystemRawArtifactStore &
  operator=(FilesystemRawArtifactStore &&) = delete;

  [[nodiscard]] StoredRawArtifact
  put_if_absent(std::string_view bytes) override;
  [[nodiscard]] std::optional<std::string>
  get_verified(const core::ContentAddressedStoreKey &key,
               const core::ContentDigest &expected_digest,
               std::uint64_t expected_size) const override;

  /// Repository-wide snapshots refreshed under the cross-process lock.
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::uint64_t retained_bytes() const;

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
  friend class detail::RetainedBundleReloadAuthority;
};

[[nodiscard]] bool
verify_retained_bundle_identity(const RetainedBundle &bundle);

/// Exact immutable coordinates for one retained product artifact.
///
/// The query carries no path, alias, candidate, or active-selection input.
/// Every field must match the selected semantic envelope before any raw bytes
/// are returned.
struct RetainedProductArtifactQuery final {
  core::RetainedBundleId retained_bundle_id;
  core::ProductId product_id;
  core::RetainedProductKind product_kind;
  core::ArtifactRoleId artifact_role_id;

  friend bool operator==(const RetainedProductArtifactQuery &,
                         const RetainedProductArtifactQuery &) = default;
};

/// Sealed result of one exact semantic-product and raw-artifact verification.
///
/// The returned metadata is copied from the independently reconstructed
/// retained bundle and its exact artifact reference. `bytes()` are read from
/// the immutable raw CAS and independently checked against that reference.
class VerifiedRetainedProductArtifact final {
public:
  VerifiedRetainedProductArtifact(
      const VerifiedRetainedProductArtifact &) = default;
  VerifiedRetainedProductArtifact &
  operator=(const VerifiedRetainedProductArtifact &) = default;
  VerifiedRetainedProductArtifact(
      VerifiedRetainedProductArtifact &&) noexcept = default;
  VerifiedRetainedProductArtifact &
  operator=(VerifiedRetainedProductArtifact &&) noexcept = default;
  ~VerifiedRetainedProductArtifact() = default;

  [[nodiscard]] const RetainedProductArtifactQuery &query() const noexcept {
    return query_;
  }
  [[nodiscard]] const core::ProductContractId &
  product_contract_id() const noexcept {
    return product_contract_id_;
  }
  [[nodiscard]] const core::ProductContractRevision &
  product_contract_revision() const noexcept {
    return product_contract_revision_;
  }
  [[nodiscard]] const core::RetainedArtifactRefDTO &
  artifact_ref() const noexcept {
    return artifact_ref_;
  }
  [[nodiscard]] std::string_view bytes() const noexcept { return bytes_; }

private:
  VerifiedRetainedProductArtifact(
      RetainedProductArtifactQuery query,
      core::ProductContractId product_contract_id,
      core::ProductContractRevision product_contract_revision,
      core::RetainedArtifactRefDTO artifact_ref, std::string bytes)
      : query_(std::move(query)),
        product_contract_id_(std::move(product_contract_id)),
        product_contract_revision_(std::move(product_contract_revision)),
        artifact_ref_(std::move(artifact_ref)),
        bytes_(std::move(bytes)) {}

  RetainedProductArtifactQuery query_;
  core::ProductContractId product_contract_id_;
  core::ProductContractRevision product_contract_revision_;
  core::RetainedArtifactRefDTO artifact_ref_;
  std::string bytes_;

  friend class FilesystemVerifiedRetainedProductArtifactSource;
};

/// Exact verified retained-product artifact point-read capability.
///
/// Unknown bundle/product/kind/role tuples return no value. A present
/// semantic or raw value that cannot be verified fails closed.
class IVerifiedRetainedProductArtifactSource {
public:
  virtual ~IVerifiedRetainedProductArtifactSource() = default;

  [[nodiscard]] virtual std::optional<VerifiedRetainedProductArtifact>
  get_verified(const RetainedProductArtifactQuery &query) const = 0;
};

/// Narrow lower-level verified-bundle lookup capability.
///
/// The returned value is sealed and was independently reconstructed from the
/// complete canonical envelope. Every raw content reference present in that
/// envelope must also have been independently resolved and digest-verified
/// before return. A missing bundle returns null; present but unverifiable
/// content fails closed. Callers supply a strong bundle ID, so this capability
/// is only a dependency of the later zero-input, build-owned canonical-empty
/// admission source; it is not that Manager admission boundary. It exposes no
/// candidate, active, stage, or Apply effect.
class IVerifiedRetainedBundleSource {
public:
  virtual ~IVerifiedRetainedBundleSource() = default;

  [[nodiscard]] virtual std::shared_ptr<const RetainedBundle>
  get_verified(const core::RetainedBundleId &retained_bundle_id) const = 0;
};

/// Semantic retained-bundle repository port. It owns immutable envelopes and
/// never owns an active/candidate alias or Apply selection.
class IRetainedBundleRepository {
public:
  virtual ~IRetainedBundleRepository() = default;

  [[nodiscard]] virtual bool
  put_if_absent(std::shared_ptr<const RetainedBundle> bundle) = 0;
  [[nodiscard]] virtual std::shared_ptr<const RetainedBundle>
  get_verified_envelope(
      const core::RetainedBundleId &retained_bundle_id) const = 0;
};

/// Durable immutable semantic retained-bundle repository.
///
/// Each bundle is published by strong RetainedBundleId after its exact
/// canonical envelope has been re-parsed and fully verified. Reopen performs
/// the same verification for every bounded entry. The repository has no
/// mutable current/active alias. A verified semantic envelope alone is not a
/// fully verified bundle: referenced raw artifacts are checked separately by
/// FilesystemVerifiedRetainedBundleSource.
///
/// `trusted_root` has the same deployment-owned, stable-hierarchy threat
/// model as FilesystemRawArtifactStore. First creation and durability of that
/// root are bootstrap responsibilities; the repository durably creates only
/// its owned descendants and files.
class FilesystemRetainedBundleRepository final
    : public IRetainedBundleRepository {
public:
  explicit FilesystemRetainedBundleRepository(
      std::filesystem::path trusted_root,
      std::size_t max_bundle_count = kMaxDurableRetainedBundleCount,
      std::uint64_t max_total_bytes = kMaxDurableRetainedBundleBytes,
      std::uint64_t max_single_bundle_bytes =
          kMaxDurableRetainedBundleBytes);
  ~FilesystemRetainedBundleRepository() override;

  FilesystemRetainedBundleRepository(
      const FilesystemRetainedBundleRepository &) = delete;
  FilesystemRetainedBundleRepository &
  operator=(const FilesystemRetainedBundleRepository &) = delete;
  FilesystemRetainedBundleRepository(
      FilesystemRetainedBundleRepository &&) = delete;
  FilesystemRetainedBundleRepository &
  operator=(FilesystemRetainedBundleRepository &&) = delete;

  [[nodiscard]] bool
  put_if_absent(std::shared_ptr<const RetainedBundle> bundle) override;
  [[nodiscard]] std::shared_ptr<const RetainedBundle>
  get_verified_envelope(
      const core::RetainedBundleId &retained_bundle_id) const override;

  /// Repository-wide snapshots refreshed under the cross-process lock.
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::uint64_t retained_bytes() const;

private:
  struct Storage;
  std::unique_ptr<Storage> storage_;
};

/// Filesystem-backed lower-level verified retained-bundle lookup.
///
/// This narrow capability point-reads the selected complete semantic envelope,
/// then resolves and independently verifies every retained catalog, schema,
/// and product-artifact reference against the same trusted package root.
/// Construction validates the fixed package directories without creating,
/// locking, syncing, cleaning, or enumerating them.
///
/// This lookup performs no canonical-empty admission or selection and exposes
/// no candidate, active, stage, Apply, bootstrap, scene, process, or runtime
/// effect.
class FilesystemVerifiedRetainedBundleSource final
    : public IVerifiedRetainedBundleSource {
public:
  explicit FilesystemVerifiedRetainedBundleSource(
      std::filesystem::path trusted_package_root);
  ~FilesystemVerifiedRetainedBundleSource() override = default;

  FilesystemVerifiedRetainedBundleSource(
      const FilesystemVerifiedRetainedBundleSource &) = delete;
  FilesystemVerifiedRetainedBundleSource &
  operator=(const FilesystemVerifiedRetainedBundleSource &) = delete;
  FilesystemVerifiedRetainedBundleSource(
      FilesystemVerifiedRetainedBundleSource &&) = delete;
  FilesystemVerifiedRetainedBundleSource &
  operator=(FilesystemVerifiedRetainedBundleSource &&) = delete;

  [[nodiscard]] std::shared_ptr<const RetainedBundle>
  get_verified(
      const core::RetainedBundleId &retained_bundle_id) const override;

private:
  std::filesystem::path trusted_package_root_;
};

/// Filesystem-backed exact retained-product artifact point reader.
///
/// Construction validates the fixed package directories without creating or
/// enumerating them. Each call point-reads one strong bundle identity, matches
/// one exact product/kind/role tuple in its verified semantic envelope, and
/// returns only the independently verified selected bytes. It has no path,
/// listing, alias, candidate, Apply, or mutation surface.
class FilesystemVerifiedRetainedProductArtifactSource final
    : public IVerifiedRetainedProductArtifactSource {
public:
  explicit FilesystemVerifiedRetainedProductArtifactSource(
      std::filesystem::path trusted_package_root);
  ~FilesystemVerifiedRetainedProductArtifactSource() override = default;

  FilesystemVerifiedRetainedProductArtifactSource(
      const FilesystemVerifiedRetainedProductArtifactSource &) = delete;
  FilesystemVerifiedRetainedProductArtifactSource &
  operator=(const FilesystemVerifiedRetainedProductArtifactSource &) = delete;
  FilesystemVerifiedRetainedProductArtifactSource(
      FilesystemVerifiedRetainedProductArtifactSource &&) = delete;
  FilesystemVerifiedRetainedProductArtifactSource &
  operator=(FilesystemVerifiedRetainedProductArtifactSource &&) = delete;

  [[nodiscard]] std::optional<VerifiedRetainedProductArtifact>
  get_verified(const RetainedProductArtifactQuery &query) const override;

private:
  std::filesystem::path trusted_package_root_;
};

} // namespace manyforge::deployment
