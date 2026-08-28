#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

#include "manyforge_deployment/build_manifest.hpp"
#include "manyforge_deployment/canonical_value.hpp"
#include "build_manifest_contract_internal.hpp"
#include "retained_bundle_internal.hpp"

namespace manyforge::deployment::detail {

[[nodiscard]] std::shared_ptr<const BuildQualifiedManifest>
reload_self_verified_build_manifest(BuildManifestKind kind,
                                    std::string_view canonical_bytes);

/// Sole production owner of build-qualified retention authority.
///
/// Ordinary manifest qualification accepts no caller values, paths, digests,
/// manifests, or environment variables. It verifies the build-embedded
/// behavior manifest, regenerates the compiler contract from live code-owned
/// registries, and regenerates the compiler build manifest from configure-time
/// closed build metadata.
class BuildQualifiedRetentionAuthority final {
 public:
  [[nodiscard]] static BuildQualifiedRetentionContext qualify();
  [[nodiscard]] static BuildQualifiedRetentionContext qualify_persisted(
      IBuildManifestRepository& repository);
  [[nodiscard]] static BuildQualifiedRetentionContext qualify_existing(
      const std::filesystem::path& trusted_package_root);

 private:
  [[nodiscard]] static std::shared_ptr<const BuildQualifiedManifest>
  qualify_self_verified_candidate(
      BuildManifestKind kind, const CanonicalValue& candidate);
  [[nodiscard]] static std::shared_ptr<const BuildQualifiedManifest>
  qualify_candidate(BuildManifestKind kind, const CanonicalValue& candidate);
  friend class BuildManifestTestAccess;
  friend std::shared_ptr<const BuildQualifiedManifest>
  reload_self_verified_build_manifest(
      BuildManifestKind kind, std::string_view canonical_bytes);
};

/// Uninstalled inspection/mutation seam for focused authority tests.
///
/// Values returned here are untrusted canonical candidates. Only
/// `qualify_candidate` can mint a sealed manifest, and the method always
/// compares against this exact build's independently regenerated value.
class BuildManifestTestAccess final {
 public:
  [[nodiscard]] static CanonicalValue current_candidate(
      BuildManifestKind kind);
  [[nodiscard]] static std::shared_ptr<const BuildQualifiedManifest>
  qualify_candidate(BuildManifestKind kind, const CanonicalValue& candidate);
  [[nodiscard]] static BuildQualifiedRetentionContext qualify_candidates(
      const CanonicalValue& node_environment,
      const CanonicalValue& compiler_contract,
      const CanonicalValue& compiler_build);
  [[nodiscard]] static std::shared_ptr<const RetainedBundle>
  materialize_with_candidates(
      const RetentionQualifiedDeployment& qualified,
      const CanonicalValue& node_environment,
      const CanonicalValue& compiler_contract,
      const CanonicalValue& compiler_build,
      IBuildManifestRepository& manifest_repository,
      IRawArtifactStore& artifact_store);
};

[[nodiscard]] CanonicalValue current_build_manifest_candidate(
    BuildManifestKind kind);

[[nodiscard]] core::RetainedNodeEnvironmentDTO
retained_node_environment_from_manifest(
    const BuildQualifiedManifest& manifest);

}  // namespace manyforge::deployment::detail
