#pragma once

#include <cstdint>
#include <filesystem>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace manyforge::deployment {

namespace detail {
class RetainedBundleConstructionAuthority;
}

class FilesystemVerifiedRetainedProductArtifactSource;

inline constexpr std::uint64_t kMaxDurableRetainedBlobBytes =
    512ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaxDurableRetainedBundleBytes =
    512ULL * 1024ULL * 1024ULL;

class RetainedBundleError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// Immutable semantic retained-bundle value reconstructed from one exact,
/// independently verified canonical envelope.
///
/// Construction is sealed behind the deployment-owned codec. Read-only
/// consumers can inspect the complete typed value and canonical identity but
/// cannot manufacture a value or publish a candidate/active selection.
class RetainedBundle final {
 public:
  ~RetainedBundle() = default;
  RetainedBundle(const RetainedBundle&) = delete;
  RetainedBundle& operator=(const RetainedBundle&) = delete;
  RetainedBundle(RetainedBundle&&) = delete;
  RetainedBundle& operator=(RetainedBundle&&) = delete;

  [[nodiscard]] const core::RetainedBundleId&
  retained_bundle_id() const noexcept {
    return retained_bundle_id_;
  }
  [[nodiscard]] const core::RetainedBundleDTO& value() const noexcept {
    return value_;
  }
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept {
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

  friend class detail::RetainedBundleConstructionAuthority;
};

[[nodiscard]] bool
verify_retained_bundle_identity(const RetainedBundle& bundle);

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

  friend bool operator==(const RetainedProductArtifactQuery&,
                         const RetainedProductArtifactQuery&) = default;
};

/// Sealed result of one exact semantic-product and raw-artifact verification.
///
/// The returned metadata is copied from the independently reconstructed
/// retained bundle and its exact artifact reference. `bytes()` are read from
/// the immutable raw CAS and independently checked against that reference.
class VerifiedRetainedProductArtifact final {
 public:
  VerifiedRetainedProductArtifact(
      const VerifiedRetainedProductArtifact&) = default;
  VerifiedRetainedProductArtifact& operator=(
      const VerifiedRetainedProductArtifact&) = default;
  VerifiedRetainedProductArtifact(
      VerifiedRetainedProductArtifact&&) noexcept = default;
  VerifiedRetainedProductArtifact& operator=(
      VerifiedRetainedProductArtifact&&) noexcept = default;
  ~VerifiedRetainedProductArtifact() = default;

  [[nodiscard]] const RetainedProductArtifactQuery& query() const noexcept {
    return query_;
  }
  [[nodiscard]] const core::ProductContractId&
  product_contract_id() const noexcept {
    return product_contract_id_;
  }
  [[nodiscard]] const core::ProductContractRevision&
  product_contract_revision() const noexcept {
    return product_contract_revision_;
  }
  [[nodiscard]] const core::RetainedArtifactRefDTO&
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
  get_verified(const RetainedProductArtifactQuery& query) const = 0;
};

/// Narrow lower-level verified-bundle lookup capability.
///
/// The returned value is sealed and was independently reconstructed from the
/// complete canonical envelope. Every raw content reference present in that
/// envelope must also have been independently resolved and digest-verified
/// before return. A missing bundle returns null; present but unverifiable
/// content fails closed. Callers supply a strong bundle ID, so this capability
/// is only a dependency of the zero-input, build-owned canonical-empty
/// admission source; it is not that Manager admission boundary. It exposes no
/// candidate, active, stage, or Apply effect.
class IVerifiedRetainedBundleSource {
 public:
  virtual ~IVerifiedRetainedBundleSource() = default;

  [[nodiscard]] virtual std::shared_ptr<const RetainedBundle>
  get_verified(const core::RetainedBundleId& retained_bundle_id) const = 0;
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
      const FilesystemVerifiedRetainedBundleSource&) = delete;
  FilesystemVerifiedRetainedBundleSource& operator=(
      const FilesystemVerifiedRetainedBundleSource&) = delete;
  FilesystemVerifiedRetainedBundleSource(
      FilesystemVerifiedRetainedBundleSource&&) = delete;
  FilesystemVerifiedRetainedBundleSource& operator=(
      FilesystemVerifiedRetainedBundleSource&&) = delete;

  [[nodiscard]] std::shared_ptr<const RetainedBundle>
  get_verified(const core::RetainedBundleId& retained_bundle_id) const override;

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
      const FilesystemVerifiedRetainedProductArtifactSource&) = delete;
  FilesystemVerifiedRetainedProductArtifactSource& operator=(
      const FilesystemVerifiedRetainedProductArtifactSource&) = delete;
  FilesystemVerifiedRetainedProductArtifactSource(
      FilesystemVerifiedRetainedProductArtifactSource&&) = delete;
  FilesystemVerifiedRetainedProductArtifactSource& operator=(
      FilesystemVerifiedRetainedProductArtifactSource&&) = delete;

  [[nodiscard]] std::optional<VerifiedRetainedProductArtifact>
  get_verified(const RetainedProductArtifactQuery& query) const override;

 private:
  std::filesystem::path trusted_package_root_;
};

}  // namespace manyforge::deployment
