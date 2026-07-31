#pragma once

#include <cstdint>
#include <filesystem>
#include <manyforge_deployment/retained_bundle.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace manyforge::deployment {

inline constexpr std::string_view kCanonicalEmptyAdmissionApiVersion =
    "manyforge.canonical_empty_admission.v1";
inline constexpr std::uint64_t kMaxCanonicalEmptyAdmissionBytes =
    16ULL * 1024ULL;

class CanonicalEmptyAdmissionError final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Sealed proof that the exact build-owned canonical-empty package was
/// independently reconstructed and verified.
///
/// This value is intentionally neither caller-constructible nor detachable
/// from the verified bundle whose lifetime it owns. The serialized ten-field
/// admission record is available for evidence and diagnostics; it is not a
/// request DTO.
class CanonicalEmptyRetainedBundleAdmission final {
 public:
  ~CanonicalEmptyRetainedBundleAdmission() = default;

  CanonicalEmptyRetainedBundleAdmission(
      const CanonicalEmptyRetainedBundleAdmission&) = delete;
  CanonicalEmptyRetainedBundleAdmission& operator=(
      const CanonicalEmptyRetainedBundleAdmission&) = delete;
  CanonicalEmptyRetainedBundleAdmission(
      CanonicalEmptyRetainedBundleAdmission&&) = delete;
  CanonicalEmptyRetainedBundleAdmission& operator=(
      CanonicalEmptyRetainedBundleAdmission&&) = delete;

  [[nodiscard]] std::string_view api_version() const noexcept {
    return kCanonicalEmptyAdmissionApiVersion;
  }
  [[nodiscard]] const core::ContentDigest& admission_digest() const noexcept {
    return admission_digest_;
  }
  [[nodiscard]] const core::CellId& cell_id() const noexcept {
    return cell_id_;
  }
  [[nodiscard]] const core::DeploymentArtifactId&
  deployment_artifact_id() const noexcept {
    return deployment_artifact_id_;
  }
  [[nodiscard]] const core::DeploymentRevision&
  deployment_revision() const noexcept {
    return deployment_revision_;
  }
  [[nodiscard]] const core::GraphRevision& graph_revision() const noexcept {
    return graph_revision_;
  }
  [[nodiscard]] const core::RetainedBundleId&
  retained_bundle_id() const noexcept {
    return retained_bundle_id_;
  }
  [[nodiscard]] const core::ContentDigest&
  environment_digest() const noexcept {
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
  [[nodiscard]] const RetainedBundle& retained_bundle() const noexcept {
    return *retained_bundle_;
  }
  [[nodiscard]] std::string_view canonical_bytes() const noexcept {
    return canonical_bytes_;
  }

 private:
  CanonicalEmptyRetainedBundleAdmission(
      core::ContentDigest admission_digest, core::CellId cell_id,
      core::DeploymentArtifactId deployment_artifact_id,
      core::DeploymentRevision deployment_revision,
      core::GraphRevision graph_revision,
      core::RetainedBundleId retained_bundle_id,
      core::ContentDigest environment_digest,
      core::ContentDigest compiler_contract_digest,
      core::ContentDigest compiler_build_digest,
      std::shared_ptr<const RetainedBundle> retained_bundle,
      std::string canonical_bytes)
      : admission_digest_(std::move(admission_digest)),
        cell_id_(std::move(cell_id)),
        deployment_artifact_id_(std::move(deployment_artifact_id)),
        deployment_revision_(std::move(deployment_revision)),
        graph_revision_(std::move(graph_revision)),
        retained_bundle_id_(std::move(retained_bundle_id)),
        environment_digest_(std::move(environment_digest)),
        compiler_contract_digest_(std::move(compiler_contract_digest)),
        compiler_build_digest_(std::move(compiler_build_digest)),
        retained_bundle_(std::move(retained_bundle)),
        canonical_bytes_(std::move(canonical_bytes)) {
    if (retained_bundle_ == nullptr) {
      throw CanonicalEmptyAdmissionError(
          "canonical-empty admission cannot own a null retained bundle");
    }
  }

  core::ContentDigest admission_digest_;
  core::CellId cell_id_;
  core::DeploymentArtifactId deployment_artifact_id_;
  core::DeploymentRevision deployment_revision_;
  core::GraphRevision graph_revision_;
  core::RetainedBundleId retained_bundle_id_;
  core::ContentDigest environment_digest_;
  core::ContentDigest compiler_contract_digest_;
  core::ContentDigest compiler_build_digest_;
  std::shared_ptr<const RetainedBundle> retained_bundle_;
  std::string canonical_bytes_;

  friend class FilesystemCanonicalEmptyRetainedBundleSource;
};

/// Sole Manager-facing canonical-empty admission boundary.
///
/// The source has no semantic input. It either returns the exact verified
/// build-owned package or fails closed.
class ICanonicalEmptyRetainedBundleSource {
 public:
  virtual ~ICanonicalEmptyRetainedBundleSource() = default;

  [[nodiscard]] virtual CanonicalEmptyRetainedBundleAdmission
  load_verified() const = 0;
};

/// Strictly read-only point loader for one deployment-owned package root.
///
/// Construction and loading create no directory, lock, temporary file, or
/// repair. No repository is enumerated and no "latest" value is selected.
class FilesystemCanonicalEmptyRetainedBundleSource final
    : public ICanonicalEmptyRetainedBundleSource {
 public:
  explicit FilesystemCanonicalEmptyRetainedBundleSource(
      std::filesystem::path trusted_package_root);
  ~FilesystemCanonicalEmptyRetainedBundleSource() override = default;

  FilesystemCanonicalEmptyRetainedBundleSource(
      const FilesystemCanonicalEmptyRetainedBundleSource&) = delete;
  FilesystemCanonicalEmptyRetainedBundleSource& operator=(
      const FilesystemCanonicalEmptyRetainedBundleSource&) = delete;
  FilesystemCanonicalEmptyRetainedBundleSource(
      FilesystemCanonicalEmptyRetainedBundleSource&&) = delete;
  FilesystemCanonicalEmptyRetainedBundleSource& operator=(
      FilesystemCanonicalEmptyRetainedBundleSource&&) = delete;

  [[nodiscard]] CanonicalEmptyRetainedBundleAdmission
  load_verified() const override;

 private:
  std::filesystem::path trusted_package_root_;
};

}  // namespace manyforge::deployment
