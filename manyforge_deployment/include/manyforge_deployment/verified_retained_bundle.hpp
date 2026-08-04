#pragma once

#include <cstdint>
#include <filesystem>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::deployment {

namespace detail {
class RetainedBundleConstructionAuthority;
class VerifiedRetainedPlanningConstructionAuthority;
}

class FilesystemVerifiedRetainedProductArtifactSource;
class FilesystemVerifiedRetainedModelMapSource;
class FilesystemVerifiedRetainedPlanningModelSource;
class VerifiedRetainedPlanningModel;

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

/// Sealed verification result for the one build-qualified reference runtime
/// row.
///
/// This value exists only after the semantic bundle and both runtime product
/// artifacts have been independently re-read and checked as one exact pair.
/// It exposes the immutable bytes and identities needed by the Component
/// Manager's adapter admission without exposing compiler IR or a generic
/// executable tuple.
class VerifiedRetainedRuntimeUnitProduct final {
 public:
  VerifiedRetainedRuntimeUnitProduct(
      const VerifiedRetainedRuntimeUnitProduct&) = default;
  VerifiedRetainedRuntimeUnitProduct& operator=(
      const VerifiedRetainedRuntimeUnitProduct&) = default;
  VerifiedRetainedRuntimeUnitProduct(
      VerifiedRetainedRuntimeUnitProduct&&) noexcept = default;
  VerifiedRetainedRuntimeUnitProduct& operator=(
      VerifiedRetainedRuntimeUnitProduct&&) noexcept = default;
  ~VerifiedRetainedRuntimeUnitProduct() = default;

  [[nodiscard]] const core::RetainedBundleId&
  retained_bundle_id() const noexcept {
    return retained_bundle_id_;
  }
  [[nodiscard]] const core::RuntimeUnitId& runtime_unit_id() const noexcept {
    return runtime_unit_id_;
  }
  [[nodiscard]] const core::ProductId& product_id() const noexcept {
    return product_id_;
  }
  [[nodiscard]] const core::ContentDigest& product_identity() const noexcept {
    return product_identity_;
  }
  [[nodiscard]] const core::ContentDigest&
  configuration_identity() const noexcept {
    return configuration_identity_;
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
  launchable_artifact_ref() const noexcept {
    return launchable_artifact_ref_;
  }
  [[nodiscard]] std::string_view launchable_bytes() const noexcept {
    return launchable_bytes_;
  }
  [[nodiscard]] std::string_view operating_system() const noexcept {
    return operating_system_;
  }
  [[nodiscard]] std::string_view shell_contract() const noexcept {
    return shell_contract_;
  }
  [[nodiscard]] std::string_view runtime_role_id() const noexcept {
    return runtime_role_id_;
  }
  [[nodiscard]] std::string_view status_endpoint_role_id() const noexcept {
    return status_endpoint_role_id_;
  }
  [[nodiscard]] std::string_view
  process_adapter_contract_id() const noexcept {
    return process_adapter_contract_id_;
  }
  [[nodiscard]] std::string_view
  process_adapter_contract_revision() const noexcept {
    return process_adapter_contract_revision_;
  }
  [[nodiscard]] std::string_view retrieval_contract_id() const noexcept {
    return retrieval_contract_id_;
  }
  [[nodiscard]] std::string_view
  retrieval_contract_revision() const noexcept {
    return retrieval_contract_revision_;
  }

 private:
  VerifiedRetainedRuntimeUnitProduct(
      core::RetainedBundleId retained_bundle_id,
      core::RuntimeUnitId runtime_unit_id, core::ProductId product_id,
      core::ContentDigest product_identity,
      core::ContentDigest configuration_identity,
      core::ProductContractId product_contract_id,
      core::ProductContractRevision product_contract_revision,
      core::RetainedArtifactRefDTO launchable_artifact_ref,
      std::string launchable_bytes, std::string operating_system,
      std::string shell_contract, std::string runtime_role_id,
      std::string status_endpoint_role_id,
      std::string process_adapter_contract_id,
      std::string process_adapter_contract_revision,
      std::string retrieval_contract_id,
      std::string retrieval_contract_revision)
      : retained_bundle_id_(std::move(retained_bundle_id)),
        runtime_unit_id_(std::move(runtime_unit_id)),
        product_id_(std::move(product_id)),
        product_identity_(std::move(product_identity)),
        configuration_identity_(std::move(configuration_identity)),
        product_contract_id_(std::move(product_contract_id)),
        product_contract_revision_(std::move(product_contract_revision)),
        launchable_artifact_ref_(std::move(launchable_artifact_ref)),
        launchable_bytes_(std::move(launchable_bytes)),
        operating_system_(std::move(operating_system)),
        shell_contract_(std::move(shell_contract)),
        runtime_role_id_(std::move(runtime_role_id)),
        status_endpoint_role_id_(std::move(status_endpoint_role_id)),
        process_adapter_contract_id_(std::move(process_adapter_contract_id)),
        process_adapter_contract_revision_(
            std::move(process_adapter_contract_revision)),
        retrieval_contract_id_(std::move(retrieval_contract_id)),
        retrieval_contract_revision_(std::move(retrieval_contract_revision)) {}

  core::RetainedBundleId retained_bundle_id_;
  core::RuntimeUnitId runtime_unit_id_;
  core::ProductId product_id_;
  core::ContentDigest product_identity_;
  core::ContentDigest configuration_identity_;
  core::ProductContractId product_contract_id_;
  core::ProductContractRevision product_contract_revision_;
  core::RetainedArtifactRefDTO launchable_artifact_ref_;
  std::string launchable_bytes_;
  std::string operating_system_;
  std::string shell_contract_;
  std::string runtime_role_id_;
  std::string status_endpoint_role_id_;
  std::string process_adapter_contract_id_;
  std::string process_adapter_contract_revision_;
  std::string retrieval_contract_id_;
  std::string retrieval_contract_revision_;

  friend std::optional<VerifiedRetainedRuntimeUnitProduct>
  verify_reference_runtime_unit_product(
      const RetainedBundle&, const core::RuntimeUnitId&,
      const IVerifiedRetainedProductArtifactSource&);
};

/// Verify one exact reference runtime product for an I4 consumer.
///
/// Missing runtime-unit coordinates return no value. A present bundle,
/// product, manifest, or launchable artifact that fails any exact contract,
/// subject, provenance, or cross-artifact check fails closed.
[[nodiscard]] std::optional<VerifiedRetainedRuntimeUnitProduct>
verify_reference_runtime_unit_product(
    const RetainedBundle& bundle, const core::RuntimeUnitId& runtime_unit_id,
    const IVerifiedRetainedProductArtifactSource& artifact_source);

/// Exact coordinates for the generic composed-model map retained beside one
/// planning model. Product kind and artifact role are fixed by this typed
/// capability rather than supplied by a runtime consumer.
struct RetainedModelMapQuery final {
  core::RetainedBundleId retained_bundle_id;
  core::ProductId product_id;

  friend bool operator==(const RetainedModelMapQuery&,
                         const RetainedModelMapQuery&) = default;
};

/// Sealed typed model/control namespace reconstructed from one exact retained
/// symbol/role-map product.
///
/// This is the public runtime boundary. It exposes compiler-owned identities
/// and role resolutions without exposing CanonicalValue, artifact bytes, or a
/// CompilerQualifiedCellIr side channel. Every row was independently parsed,
/// checked against the retained graph, and bound to the queried bundle.
class VerifiedRetainedModelMap final {
 public:
  VerifiedRetainedModelMap(const VerifiedRetainedModelMap&) = default;
  VerifiedRetainedModelMap& operator=(
      const VerifiedRetainedModelMap&) = default;
  VerifiedRetainedModelMap(VerifiedRetainedModelMap&&) noexcept = default;
  VerifiedRetainedModelMap& operator=(
      VerifiedRetainedModelMap&&) noexcept = default;
  ~VerifiedRetainedModelMap() = default;

  [[nodiscard]] const RetainedModelMapQuery& query() const noexcept {
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
  [[nodiscard]] const CompositionCellFrame& cell_frame() const noexcept {
    return cell_frame_;
  }
  [[nodiscard]] const std::vector<CompositionModelOccurrence>&
  model_occurrences() const noexcept {
    return model_occurrences_;
  }
  [[nodiscard]] const std::vector<OccurrenceSymbolRewrite>& symbols()
      const noexcept {
    return symbols_;
  }
  [[nodiscard]] const std::vector<OccurrenceReferenceRewrite>& references()
      const noexcept {
    return references_;
  }
  [[nodiscard]] const std::vector<CompositionModelCoordinate>& coordinates()
      const noexcept {
    return coordinates_;
  }
  [[nodiscard]] const std::vector<CompositionControlBindingCoordinateMap>&
  control_bindings() const noexcept {
    return control_bindings_;
  }
  [[nodiscard]] const std::vector<CompositionTargetResolution>& targets()
      const noexcept {
    return targets_;
  }

  [[nodiscard]] const CompositionTargetResolution* find_target(
      const core::ControllableComponentId& component_id,
      const core::CommandTargetId& command_target_id) const noexcept;

  /// Returns the sole binding for a logical component/target pair. Missing
  /// and ambiguous pairs both return null; a future multi-route consumer must
  /// select by an additional retained binding/capability coordinate instead
  /// of relying on artifact order.
  [[nodiscard]] const CompositionControlBindingCoordinateMap* find_binding(
      const core::ControllableComponentId& component_id,
      const core::CommandTargetId& command_target_id) const noexcept;

  /// Fails closed if a consumer attempts to combine this namespace with a
  /// planning product selected from another immutable bundle.
  void require_same_bundle_as(
      const VerifiedRetainedPlanningModel& planning_model) const;

 private:
  VerifiedRetainedModelMap(
      RetainedModelMapQuery query,
      core::ProductContractId product_contract_id,
      core::ProductContractRevision product_contract_revision,
      CompositionCellFrame cell_frame,
      std::vector<CompositionModelOccurrence> model_occurrences,
      std::vector<OccurrenceSymbolRewrite> symbols,
      std::vector<OccurrenceReferenceRewrite> references,
      std::vector<CompositionModelCoordinate> coordinates,
      std::vector<CompositionControlBindingCoordinateMap> control_bindings,
      std::vector<CompositionTargetResolution> targets)
      : query_(std::move(query)),
        product_contract_id_(std::move(product_contract_id)),
        product_contract_revision_(std::move(product_contract_revision)),
        cell_frame_(std::move(cell_frame)),
        model_occurrences_(std::move(model_occurrences)),
        symbols_(std::move(symbols)),
        references_(std::move(references)),
        coordinates_(std::move(coordinates)),
        control_bindings_(std::move(control_bindings)),
        targets_(std::move(targets)) {}

  RetainedModelMapQuery query_;
  core::ProductContractId product_contract_id_;
  core::ProductContractRevision product_contract_revision_;
  CompositionCellFrame cell_frame_;
  std::vector<CompositionModelOccurrence> model_occurrences_;
  std::vector<OccurrenceSymbolRewrite> symbols_;
  std::vector<OccurrenceReferenceRewrite> references_;
  std::vector<CompositionModelCoordinate> coordinates_;
  std::vector<CompositionControlBindingCoordinateMap> control_bindings_;
  std::vector<CompositionTargetResolution> targets_;

  friend class FilesystemVerifiedRetainedModelMapSource;
};

class IVerifiedRetainedModelMapSource {
 public:
  virtual ~IVerifiedRetainedModelMapSource() = default;

  [[nodiscard]] virtual std::optional<VerifiedRetainedModelMap>
  get_verified(const RetainedModelMapQuery& query) const = 0;
};

/// Exact immutable coordinates for one retained planning model.
///
/// Product kind and placement are deliberately not caller-selectable: this
/// capability resolves only the cell-wide planning/collision product.
struct RetainedPlanningModelQuery final {
  core::RetainedBundleId retained_bundle_id;
  core::ProductId product_id;

  friend bool operator==(const RetainedPlanningModelQuery&,
                         const RetainedPlanningModelQuery&) = default;
};

/// One owned, independently reverified non-XML resource from a graph-selected
/// model asset. `logical_uri()` is the compiler-owned revision-scoped URI
/// retained by the planning product, not the source ModelAsset URI.
///
/// Construction is sealed behind the deployment reader. Copy and move are
/// safe because every view below refers to storage owned by this value.
class VerifiedRetainedPlanningResource final {
 public:
  VerifiedRetainedPlanningResource(
      const VerifiedRetainedPlanningResource&) = default;
  VerifiedRetainedPlanningResource& operator=(
      const VerifiedRetainedPlanningResource&) = default;
  VerifiedRetainedPlanningResource(
      VerifiedRetainedPlanningResource&&) noexcept = default;
  VerifiedRetainedPlanningResource& operator=(
      VerifiedRetainedPlanningResource&&) noexcept = default;
  ~VerifiedRetainedPlanningResource() = default;

  [[nodiscard]] std::string_view logical_uri() const noexcept {
    return logical_uri_;
  }
  [[nodiscard]] std::string_view media_type() const noexcept {
    return media_type_;
  }
  [[nodiscard]] std::uint64_t byte_size() const noexcept {
    return byte_size_;
  }
  [[nodiscard]] const core::ContentDigest&
  content_digest() const noexcept {
    return content_digest_;
  }
  [[nodiscard]] const core::ContentAddressedStoreKey&
  content_addressed_store_key() const noexcept {
    return content_addressed_store_key_;
  }
  [[nodiscard]] std::string_view bytes() const noexcept {
    return bytes_;
  }

 private:
  VerifiedRetainedPlanningResource(
      std::string logical_uri, std::string media_type,
      std::uint64_t byte_size, core::ContentDigest content_digest,
      core::ContentAddressedStoreKey content_addressed_store_key,
      std::string bytes)
      : logical_uri_(std::move(logical_uri)),
        media_type_(std::move(media_type)),
        byte_size_(byte_size),
        content_digest_(std::move(content_digest)),
        content_addressed_store_key_(
            std::move(content_addressed_store_key)),
        bytes_(std::move(bytes)) {}

  std::string logical_uri_;
  std::string media_type_;
  std::uint64_t byte_size_;
  core::ContentDigest content_digest_;
  core::ContentAddressedStoreKey content_addressed_store_key_;
  std::string bytes_;

  friend class detail::VerifiedRetainedPlanningConstructionAuthority;
};

/// Sealed, owned planning input reconstructed from one exact retained bundle.
///
/// The URDF/SRDF bytes are the exact compiled planning-product artifacts.
/// Resources are the canonical-sorted non-XML portion of the complete
/// graph-selected ModelAsset source closure projected through the retained
/// compiler-owned resource map. The reader verifies the source XML as well,
/// but does not duplicate those entrypoints in `resources()`.
class VerifiedRetainedPlanningModel final {
 public:
  VerifiedRetainedPlanningModel(
      const VerifiedRetainedPlanningModel&) = default;
  VerifiedRetainedPlanningModel& operator=(
      const VerifiedRetainedPlanningModel&) = default;
  VerifiedRetainedPlanningModel(
      VerifiedRetainedPlanningModel&&) noexcept = default;
  VerifiedRetainedPlanningModel& operator=(
      VerifiedRetainedPlanningModel&&) noexcept = default;
  ~VerifiedRetainedPlanningModel() = default;

  [[nodiscard]] const RetainedPlanningModelQuery&
  query() const noexcept {
    return query_;
  }
  [[nodiscard]] std::string_view urdf_bytes() const noexcept {
    return urdf_bytes_;
  }
  [[nodiscard]] std::string_view srdf_bytes() const noexcept {
    return srdf_bytes_;
  }
  [[nodiscard]] const std::vector<VerifiedRetainedPlanningResource>&
  resources() const noexcept {
    return resources_;
  }

 private:
  VerifiedRetainedPlanningModel(
      RetainedPlanningModelQuery query, std::string urdf_bytes,
      std::string srdf_bytes,
      std::vector<VerifiedRetainedPlanningResource> resources)
      : query_(std::move(query)),
        urdf_bytes_(std::move(urdf_bytes)),
        srdf_bytes_(std::move(srdf_bytes)),
        resources_(std::move(resources)) {}

  RetainedPlanningModelQuery query_;
  std::string urdf_bytes_;
  std::string srdf_bytes_;
  std::vector<VerifiedRetainedPlanningResource> resources_;

  friend class FilesystemVerifiedRetainedPlanningModelSource;
};

/// Exact retained planning-model point-read capability.
///
/// Unknown bundle/product coordinates return no value. Present semantic or
/// raw content that cannot be independently verified fails closed.
class IVerifiedRetainedPlanningModelSource {
 public:
  virtual ~IVerifiedRetainedPlanningModelSource() = default;

  [[nodiscard]] virtual std::optional<VerifiedRetainedPlanningModel>
  get_verified(const RetainedPlanningModelQuery& query) const = 0;
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

/// Filesystem-backed exact typed composed-model map reader.
///
/// The source reloads one exact bundle, fixes product kind and artifact role,
/// verifies the raw map, binds it to the retained graph, and returns only the
/// sealed typed namespace. It has no raw-map, listing, alias, or mutation
/// surface.
class FilesystemVerifiedRetainedModelMapSource final
    : public IVerifiedRetainedModelMapSource {
 public:
  explicit FilesystemVerifiedRetainedModelMapSource(
      std::filesystem::path trusted_package_root);
  ~FilesystemVerifiedRetainedModelMapSource() override = default;

  FilesystemVerifiedRetainedModelMapSource(
      const FilesystemVerifiedRetainedModelMapSource&) = delete;
  FilesystemVerifiedRetainedModelMapSource& operator=(
      const FilesystemVerifiedRetainedModelMapSource&) = delete;
  FilesystemVerifiedRetainedModelMapSource(
      FilesystemVerifiedRetainedModelMapSource&&) = delete;
  FilesystemVerifiedRetainedModelMapSource& operator=(
      FilesystemVerifiedRetainedModelMapSource&&) = delete;

  [[nodiscard]] std::optional<VerifiedRetainedModelMap>
  get_verified(const RetainedModelMapQuery& query) const override;

 private:
  std::filesystem::path trusted_package_root_;
};

/// Filesystem-backed exact retained planning-model point reader.
///
/// This capability independently reloads one exact semantic bundle, selects
/// only its cell planning/collision product, resolves exactly the
/// graph-selected ModelAsset catalog/source closure, re-runs catalog and
/// source qualification, and returns owned values. It has no path, listing,
/// alias, candidate, Apply, ambient-package, or mutation surface.
class FilesystemVerifiedRetainedPlanningModelSource final
    : public IVerifiedRetainedPlanningModelSource {
 public:
  explicit FilesystemVerifiedRetainedPlanningModelSource(
      std::filesystem::path trusted_package_root);
  ~FilesystemVerifiedRetainedPlanningModelSource() override = default;

  FilesystemVerifiedRetainedPlanningModelSource(
      const FilesystemVerifiedRetainedPlanningModelSource&) = delete;
  FilesystemVerifiedRetainedPlanningModelSource& operator=(
      const FilesystemVerifiedRetainedPlanningModelSource&) = delete;
  FilesystemVerifiedRetainedPlanningModelSource(
      FilesystemVerifiedRetainedPlanningModelSource&&) = delete;
  FilesystemVerifiedRetainedPlanningModelSource& operator=(
      FilesystemVerifiedRetainedPlanningModelSource&&) = delete;

  [[nodiscard]] std::optional<VerifiedRetainedPlanningModel>
  get_verified(const RetainedPlanningModelQuery& query) const override;

 private:
  std::filesystem::path trusted_package_root_;
};

}  // namespace manyforge::deployment
