#pragma once

#include <filesystem>
#include <manyforge_core/types/strong_id.hpp>
#include <utility>

namespace manyforge::deployment::detail {

/// Sealed proof that one package's producer-manifest closure was loaded by
/// exact content-addressed identity, self-verified, and cross-bound.
class PackageAdmissionProfile final {
 public:
  [[nodiscard]] const core::ContentDigest& environment_digest() const noexcept {
    return environment_digest_;
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
  PackageAdmissionProfile(
      core::ContentDigest environment_digest,
      core::ContentDigest compiler_contract_digest,
      core::ContentDigest compiler_build_digest)
      : environment_digest_(std::move(environment_digest)),
        compiler_contract_digest_(std::move(compiler_contract_digest)),
        compiler_build_digest_(std::move(compiler_build_digest)) {}

  core::ContentDigest environment_digest_;
  core::ContentDigest compiler_contract_digest_;
  core::ContentDigest compiler_build_digest_;

  friend PackageAdmissionProfile verify_package_admission_profile(
      const std::filesystem::path& trusted_package_root,
      const core::ContentDigest& environment_digest,
      const core::ContentDigest& compiler_contract_digest,
      const core::ContentDigest& compiler_build_digest);
};

/// Read and verify the exact producer manifests named by one admission record.
///
/// This operation is bounded and read-only. It never compares package bytes
/// with the reader build, creates files, repairs state, or selects a latest
/// manifest. Missing, malformed, noncanonical, self-inconsistent, or cross-
/// bound files fail closed.
[[nodiscard]] PackageAdmissionProfile verify_package_admission_profile(
    const std::filesystem::path& trusted_package_root,
    const core::ContentDigest& environment_digest,
    const core::ContentDigest& compiler_contract_digest,
    const core::ContentDigest& compiler_build_digest);

}  // namespace manyforge::deployment::detail
