#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "manyforge_deployment/build_manifest.hpp"
#include "manyforge_deployment/retention_qualified_deployment.hpp"
#include "manyforge_deployment/retained_bundle.hpp"

namespace manyforge::deployment::detail {

class RetentionMaterializerTestAccess;
class BuildQualifiedRetentionAuthority;
class BuildManifestTestAccess;

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
      IRawArtifactStore& artifact_store,
      IRetainedBundleRepository& bundle_repository);

  /// Private, explicitly non-authoritative fixture path retained only for the
  /// bounded mechanical tests that preceded C1.0.
  [[nodiscard]] static std::shared_ptr<const RetainedBundle> materialize(
      const RetentionQualifiedDeployment& qualified,
      const BuildQualifiedRetentionContext& context,
      IRawArtifactStore& artifact_store);

 private:
  [[nodiscard]] static std::shared_ptr<const RetainedBundle>
  materialize_prepared(
      const RetentionQualifiedDeployment& qualified,
      const BuildQualifiedRetentionContext& context,
      std::string_view urdf, std::string_view srdf,
      const BuildQualifiedProductContract& contract,
      IRawArtifactStore& artifact_store);
};

/// Sole reconstruction authority for durable semantic repository bytes.
///
/// Reload reparses the exact canonical envelope, verifies the complete
/// currently qualified retained-bundle projection and semantic identity, and
/// only then reconstructs the sealed RetainedBundle value.
class RetainedBundleReloadAuthority final {
 public:
  [[nodiscard]] static std::shared_ptr<const RetainedBundle> reload(
      std::string_view canonical_bytes);
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

[[nodiscard]] CanonicalValue
retained_product_projection(const core::RetainedProductDTO &product);

/// Domain-separated digest used by the product's sole provenance output row.
[[nodiscard]] core::ContentDigest
retained_product_output_digest(const core::RetainedProductDTO &product);

}  // namespace manyforge::deployment::detail
