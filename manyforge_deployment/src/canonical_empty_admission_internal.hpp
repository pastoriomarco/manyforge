#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "manyforge_deployment/canonical_empty_admission.hpp"
#include "manyforge_deployment/deployment_artifact_codec.hpp"

namespace manyforge::deployment::detail {

struct CanonicalEmptyAdmissionRecord final {
  std::string canonical_bytes;
  std::string admission_digest;
  std::string cell_id;
  std::string deployment_artifact_id;
  std::string deployment_revision;
  std::string graph_revision;
  std::string retained_bundle_id;
  std::string environment_digest;
  std::string compiler_contract_digest;
  std::string compiler_build_digest;
};

[[nodiscard]] std::filesystem::path
normalize_canonical_empty_package_root(const std::filesystem::path &root);

[[nodiscard]] CanonicalEmptyAdmissionRecord
make_canonical_empty_admission_record(const RetainedBundle &bundle);

[[nodiscard]] CanonicalEmptyAdmissionRecord
parse_canonical_empty_admission_record(std::string_view bytes);

[[nodiscard]] RevisionVerifiedDeploymentArtifact
load_canonical_empty_fixed_deployment();

void require_canonical_empty_bundle_matches_authorities(
    const RetainedBundle &bundle,
    const RevisionVerifiedDeploymentArtifact &deployment,
    const core::RetainedNodeEnvironmentDTO &node_environment,
    const core::ContentDigest &compiler_contract_digest,
    const core::ContentDigest &compiler_build_digest,
    const CanonicalEmptyAdmissionRecord *record);

[[nodiscard]] std::optional<std::string>
read_canonical_empty_admission_if_present(
    const std::filesystem::path &root,
    const std::filesystem::path &admission_filename =
        "canonical-empty-admission.json");

/// Deterministically derives a structurally empty deployment for the source
/// cell while retaining its one exact native execution runtime declaration.
[[nodiscard]] RevisionVerifiedDeploymentArtifact
derive_cell_scoped_canonical_empty_deployment(
    const RevisionVerifiedDeploymentArtifact &first_deployment,
    std::span<const QualifiedRuntimeProfile> qualified_runtime_profiles);

/// Publishes and independently reloads one exact cell-scoped admission in an
/// already materialized package root.
[[nodiscard]] CanonicalEmptyRetainedBundleAdmission
publish_cell_scoped_canonical_empty_admission(
    const std::filesystem::path &package_root,
    const std::filesystem::path &admission_filename,
    const RetainedBundle &canonical_empty_bundle);

/// Builds one package from the exact embedded deployment and then reloads it
/// through the production read-only admission source.
///
/// The output root is placement only; every semantic identity is build-owned.
[[nodiscard]] CanonicalEmptyRetainedBundleAdmission
generate_canonical_empty_package(const std::filesystem::path &output_root);

} // namespace manyforge::deployment::detail
