#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "manyforge_deployment/build_manifest.hpp"
#include "manyforge_deployment/retention_qualified_deployment.hpp"
#include "manyforge_deployment/retained_bundle.hpp"
#include "manyforge_deployment/structural_model_preparation.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment::detail {

class RetentionMaterializerTestAccess;
class BuildQualifiedRetentionAuthority;
class BuildManifestTestAccess;

/// Exact read-only input-artifact source used before I3 creates any retained
/// output.
///
/// This is deliberately distinct from IRawArtifactStore: source/catalog bytes
/// may live in an independently qualified importer store, and the materializer
/// must not assume that its output CAS is also the source of compiler inputs.
/// A null expected size means "bounded by the implementation's fixed input
/// limit"; digest and key verification remain mandatory.
class ICompilerInputArtifactSource {
 public:
  virtual ~ICompilerInputArtifactSource() = default;

  [[nodiscard]] virtual std::optional<std::string> get_verified(
      const core::ContentAddressedStoreKey& key,
      const core::ContentDigest& expected_digest,
      std::optional<std::uint64_t> expected_size) const = 0;
};

/// Explicit no-input source for the ordinary canonical-empty path.
///
/// It is not a fallback: any deployment that requires a catalog or model
/// source artifact fails immediately when this source is selected.
class RejectingCompilerInputArtifactSource final
    : public ICompilerInputArtifactSource {
 public:
  [[nodiscard]] std::optional<std::string> get_verified(
      const core::ContentAddressedStoreKey&,
      const core::ContentDigest&,
      std::optional<std::uint64_t>) const override {
    return std::nullopt;
  }
};

/// Explicit no-catalog-output store for the canonical-empty path.
///
/// It creates no unused durable namespace. Any attempt to retain a catalog
/// referent through this path is a logic error rather than a fallback to the
/// digest-derived raw CAS.
class RejectingCatalogArtifactStore final : public ICatalogArtifactStore {
 public:
  [[nodiscard]] StoredCatalogArtifact put_if_absent(
      const core::ContentAddressedStoreKey&,
      const core::ContentDigest&, std::string_view) override {
    throw std::logic_error(
        "canonical-empty retention cannot publish a catalog artifact");
  }

  [[nodiscard]] std::optional<std::string> get_verified(
      const core::ContentAddressedStoreKey&,
      const core::ContentDigest&,
      std::optional<std::uint64_t>) const override {
    return std::nullopt;
  }
};

/// Exact offline build/environment evidence accepted by the current I3
/// retained-bundle materializer.
///
/// The sole production producer is BuildQualifiedRetentionAuthority. It
/// regenerates and requalifies the closed node-environment, compiler-contract,
/// and compiler-build manifests from build-owned inputs; callers cannot supply
/// digests, paths, branches, tags, or manifest bytes as authority.
class BuildQualifiedRetentionContext final {
 public:
  BuildQualifiedRetentionContext(const BuildQualifiedRetentionContext&) =
      default;
  BuildQualifiedRetentionContext& operator=(
      const BuildQualifiedRetentionContext&) = default;

  [[nodiscard]] const core::RetainedNodeEnvironmentDTO& node_environment()
      const noexcept {
    return node_environment_;
  }
  [[nodiscard]] const core::ContentDigest& compiler_contract_digest()
      const noexcept {
    return compiler_contract_digest_;
  }
  [[nodiscard]] const core::ContentDigest& compiler_build_digest()
      const noexcept {
    return compiler_build_digest_;
  }
  [[nodiscard]] std::span<
      const std::shared_ptr<const BuildQualifiedManifest>>
  manifests() const noexcept {
    return manifests_;
  }

 private:
  BuildQualifiedRetentionContext(
      core::RetainedNodeEnvironmentDTO node_environment,
      core::ContentDigest compiler_contract_digest,
      core::ContentDigest compiler_build_digest);
  BuildQualifiedRetentionContext(
      core::RetainedNodeEnvironmentDTO node_environment,
      core::ContentDigest compiler_contract_digest,
      core::ContentDigest compiler_build_digest,
      std::vector<std::shared_ptr<const BuildQualifiedManifest>> manifests);

  core::RetainedNodeEnvironmentDTO node_environment_;
  core::ContentDigest compiler_contract_digest_;
  core::ContentDigest compiler_build_digest_;
  std::vector<std::shared_ptr<const BuildQualifiedManifest>> manifests_;

  // This friend exists only in the uninstalled internal header so focused
  // tests can exercise the otherwise sealed materializer. It is not a
  // production qualification path.
  friend class RetentionMaterializerTestAccess;
  friend class BuildQualifiedRetentionAuthority;
  friend class BuildManifestTestAccess;
};

/// Sole private producer of a semantic RetainedBundle in the current slice.
///
/// The method accepts only the complete I2/deployment equality proof and the
/// separately sealed build/environment context. The currently qualified
/// subset is canonical-empty; all unsupported retained tables reject before
/// the first raw-CAS effect. URDF and SRDF are nevertheless emitted through
/// the ordinary flattened-model product contract from CompilerQualifiedCellIr.
class RetainedBundleMaterializer final {
 public:
  /// Production path. Build/environment authority is regenerated before the
  /// first manifest-repository effect, then the exact persisted closure is
  /// independently reloaded before the first raw-CAS effect. The resulting
  /// semantic bundle is durably inserted and independently reconstructed
  /// before it is returned.
  [[nodiscard]] static std::shared_ptr<const RetainedBundle>
  materialize_store_and_reload_production(
      const RetentionQualifiedDeployment& qualified,
      IBuildManifestRepository& manifest_repository,
      const ICompilerInputArtifactSource& input_artifact_source,
      std::span<const BuildQualifiedStructuralModelPreparation>
          structural_model_preparations,
      ICatalogArtifactStore& catalog_artifact_store,
      IRawArtifactStore& artifact_store,
      IRetainedBundleRepository& bundle_repository);

  /// Private, explicitly non-authoritative fixture path retained only for the
  /// bounded mechanical tests that preceded C1.0.
  [[nodiscard]] static std::shared_ptr<const RetainedBundle> materialize(
      const RetentionQualifiedDeployment& qualified,
      const BuildQualifiedRetentionContext& context,
      const ICompilerInputArtifactSource& input_artifact_source,
      std::span<const BuildQualifiedStructuralModelPreparation>
          structural_model_preparations,
      ICatalogArtifactStore& catalog_artifact_store,
      IRawArtifactStore& artifact_store);

 private:
  [[nodiscard]] static std::shared_ptr<const RetainedBundle>
  materialize_prepared(
      const RetentionQualifiedDeployment& qualified,
      const BuildQualifiedRetentionContext& context,
      std::string_view urdf, std::string_view srdf,
      std::string_view planning_resource_map,
      const BuildQualifiedProductContract& contract,
      std::optional<std::string_view> symbol_role_map,
      const BuildQualifiedProductContract* symbol_role_map_contract,
      std::span<const core::RetainedSchemaArtifactRefDTO> schema_artifacts,
      std::span<const core::RetainedSourceRecordDTO> source_records,
      std::span<const core::RetainedGeneratorRecordDTO> generator_records,
      IRawArtifactStore& artifact_store);
};

/// Compiler-internal input for one product-contract artifact role.
///
/// This type and the retention functions below are deliberately absent from
/// the installed API: only the deployment compiler may turn qualified
/// semantics into retained products.
struct RetainedArtifactInput final {
  core::ArtifactRoleId artifact_role_id;
  std::string media_type;
  std::string bytes;
};

/// Current I3 planning/collision import profile.
///
/// Generic I2 source/composition accepts and preserves valid transitive mimic
/// chains. The current retained planning/collision product is consumed by the
/// Pinocchio family, whose mimic import is qualified only for one-level
/// master/follower relations. This prepublication guard rejects a
/// mimic-of-mimic without rewriting it or creating a second naming universe.
/// It is internal because it is a product-profile constraint, not a generic
/// model-source rule.
void require_pinocchio_family_mimic_profile(
    const UrdfDocumentIr& urdf);

/// Pure complete product-contract and byte-bound preflight.
///
/// This performs no store access. Producers that retain other transitive
/// inputs before the product itself must call it first so an invalid product
/// cannot leave durable side effects.
void preflight_retained_product(
    const core::ProductId& product_id,
    core::RetainedProductKind product_kind, ProductPlacement placement,
    const core::ProductContractId& product_contract_id,
    const core::ProductContractRevision& product_contract_revision,
    std::span<const RetainedArtifactInput> artifacts);

/// Validates an exact code-owned contract and retains its raw artifacts.
///
/// Contract shape is checked completely before the first store effect.
[[nodiscard]] core::RetainedProductDTO build_and_retain_product(
    const core::ProductId &product_id, core::RetainedProductKind product_kind,
    ProductPlacement placement,
    const core::ProductContractId &product_contract_id,
    const core::ProductContractRevision &product_contract_revision,
    std::span<const RetainedArtifactInput> artifacts,
    IRawArtifactStore &artifact_store);

}  // namespace manyforge::deployment::detail
