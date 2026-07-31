#pragma once

#include <filesystem>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <utility>

namespace manyforge::deployment::detail {

/// Narrow proof that the canonical package's build-manifest closure is the
/// exact closure embedded for this reader build.
///
/// Construction is sealed behind the read-only profile verifier.  In
/// particular, callers cannot supply manifest identities or turn a digest-only
/// comparison into build authority.
class BuildQualifiedAdmissionProfileAuthority final {
 public:
  [[nodiscard]] const core::RetainedNodeEnvironmentDTO&
  node_environment() const noexcept {
    return node_environment_;
  }

  [[nodiscard]] const core::ContentDigest&
  compiler_contract_digest() const noexcept {
    return compiler_contract_digest_;
  }

  [[nodiscard]] const core::ContentDigest&
  compiler_build_digest() const noexcept {
    return compiler_build_digest_;
  }

 private:
  BuildQualifiedAdmissionProfileAuthority(
      core::RetainedNodeEnvironmentDTO node_environment,
      core::ContentDigest compiler_contract_digest,
      core::ContentDigest compiler_build_digest)
      : node_environment_(std::move(node_environment)),
        compiler_contract_digest_(std::move(compiler_contract_digest)),
        compiler_build_digest_(std::move(compiler_build_digest)) {}

  core::RetainedNodeEnvironmentDTO node_environment_;
  core::ContentDigest compiler_contract_digest_;
  core::ContentDigest compiler_build_digest_;

  friend BuildQualifiedAdmissionProfileAuthority
  verify_build_qualified_admission_profile(
      const std::filesystem::path& trusted_package_root);
};

/// Independently verifies the exact three-file build closure under one
/// existing trusted canonical-package root.
///
/// This operation is strictly read-only.  The only semantic input is package
/// placement: all manifest bytes, keys, and retained node-environment fields
/// are build-generated constants.  Missing, non-canonical, self-inconsistent,
/// stale, or cross-spliced files fail closed; nothing is created or repaired.
[[nodiscard]] BuildQualifiedAdmissionProfileAuthority
verify_build_qualified_admission_profile(
    const std::filesystem::path& trusted_package_root);

}  // namespace manyforge::deployment::detail
