#include "package_admission_profile_internal.hpp"

#include <array>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#include "build_manifest_contract_internal.hpp"
#include "content_identity_internal.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/build_manifest.hpp"
#include "manyforge_deployment/canonical_value.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "posix_durability_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

namespace posix = posix_durability;

[[nodiscard]] std::exception_ptr make_manifest_error(
    std::string_view message) {
  return std::make_exception_ptr(BuildManifestError(std::string(message)));
}

const posix::ErrorPolicy kReadOnlyProfileErrors{make_manifest_error};
constexpr posix::TrustedRootMessages kTrustedRootMessages{
    "canonical package root must not be empty",
    "cannot resolve canonical package root",
    "canonical package root must be an existing plain directory",
};
constexpr posix::ExistingDirectoryMessages kManifestDirectoryMessages{
    "cannot inspect canonical package build-manifest directory",
    "canonical package build-manifest directory must be an existing plain "
    "directory",
};
constexpr posix::ReadMessages kManifestReadMessages{
    "cannot open canonical package build manifest",
    "cannot stat canonical package build manifest",
    "canonical package build manifest violates its immutable byte bound",
    "canonical package build manifest changed during exact read",
    "canonical package build manifest grew during exact read",
};

struct RequestedManifest final {
  BuildManifestKind kind;
  const core::ContentDigest* digest;
};

struct VerifiedManifestEnvelope final {
  CanonicalValue root;
  core::ContentDigest digest;
};

[[noreturn]] void fail(std::string_view message) {
  throw BuildManifestError(std::string(message));
}

[[nodiscard]] std::filesystem::path manifest_path(
    const std::filesystem::path& manifest_root,
    const RequestedManifest& requested) {
  const std::string_view kind = to_string(requested.kind);
  const std::string_view digest = requested.digest->value();
  if (kind.empty() || !is_sha256_digest(digest)) {
    fail("canonical package admission names an invalid build-manifest key");
  }
  const std::filesystem::path kind_root =
      manifest_root / std::string(kind);
  posix::require_existing_plain_directory(
      kind_root, kReadOnlyProfileErrors, kManifestDirectoryMessages);
  return kind_root /
         (std::string(digest.substr(std::string_view("sha256:").size())) +
          ".json");
}

[[nodiscard]] ContentIdentitySpec identity_spec(BuildManifestKind kind) {
  switch (kind) {
    case BuildManifestKind::kNodeEnvironment:
      return {std::string(kNodeEnvironmentIdentityDomain),
              std::string(kNodeEnvironmentManifestApiVersion),
              "environment_digest"};
    case BuildManifestKind::kCompilerContract:
      return {std::string(kCompilerContractIdentityDomain),
              std::string(kCompilerContractManifestApiVersion),
              "compiler_contract_digest"};
    case BuildManifestKind::kCompilerBuild:
      return {std::string(kCompilerBuildIdentityDomain),
              std::string(kCompilerBuildManifestApiVersion),
              "compiler_build_digest"};
  }
  throw std::logic_error("unknown build manifest kind");
}

[[nodiscard]] const std::string& required_text_member(
    const CanonicalValue& root, std::string_view name,
    std::string_view error) {
  const CanonicalValue* member = root.find_object_member(name);
  if (member == nullptr) {
    fail(error);
  }
  const auto* text = std::get_if<std::string>(&member->storage());
  if (text == nullptr || text->empty()) {
    fail(error);
  }
  return *text;
}

[[nodiscard]] VerifiedManifestEnvelope read_exact_manifest(
    const std::filesystem::path& manifest_root,
    const RequestedManifest& requested) {
  const auto bytes = posix::read_exact_regular_file(
      manifest_path(manifest_root, requested), kMaxArtifactEncodedBytes,
      false, kReadOnlyProfileErrors, kManifestReadMessages);
  if (!bytes.has_value()) {
    throw std::logic_error("required package build manifest disappeared");
  }
  try {
    CanonicalValue root = parse_strict_json_model_yaml(*bytes).root();
    if (canonical_json(root) != *bytes) {
      fail("canonical package build manifest is not exact RFC 8785 JSON");
    }
    const ContentIdentitySpec spec = identity_spec(requested.kind);
    const std::string& api_version = required_text_member(
        root, "api_version", "canonical package build manifest omits its API version");
    if (api_version != spec.api_version) {
      fail("canonical package build manifest has an unsupported API version");
    }
    const std::string& declared = required_text_member(
        root, spec.own_field,
        "canonical package build manifest omits its content identity");
    if (!is_sha256_digest(declared) ||
        declared != requested.digest->value() ||
        !verify_content_identity(root, spec)) {
      fail("canonical package build manifest differs from its admission key");
    }
    return VerifiedManifestEnvelope{std::move(root),
                                    core::ContentDigest(declared)};
  } catch (const BuildManifestError&) {
    throw;
  } catch (const std::exception& error) {
    fail(std::string("canonical package build manifest is invalid: ") +
         error.what());
  }
}

[[nodiscard]] std::string compiler_build_contract_digest(
    const VerifiedManifestEnvelope& manifest) {
  const std::string& digest = required_text_member(
      manifest.root, "compiler_contract_digest",
      "compiler-build manifest omits its compiler-contract binding");
  if (!is_sha256_digest(digest)) {
    fail("compiler-build manifest has an invalid compiler-contract binding");
  }
  return digest;
}

}  // namespace

PackageAdmissionProfile verify_package_admission_profile(
    const std::filesystem::path& trusted_package_root,
    const core::ContentDigest& environment_digest,
    const core::ContentDigest& compiler_contract_digest,
    const core::ContentDigest& compiler_build_digest) {
  const std::filesystem::path root = posix::normalize_existing_plain_root(
      trusted_package_root, kReadOnlyProfileErrors, kTrustedRootMessages);
  const std::filesystem::path manifest_root = root / "build-manifests";
  posix::require_existing_plain_directory(
      manifest_root, kReadOnlyProfileErrors, kManifestDirectoryMessages);

  const std::array<RequestedManifest, 3U> requests{{
      {BuildManifestKind::kNodeEnvironment, &environment_digest},
      {BuildManifestKind::kCompilerContract, &compiler_contract_digest},
      {BuildManifestKind::kCompilerBuild, &compiler_build_digest},
  }};
  const VerifiedManifestEnvelope environment =
      read_exact_manifest(manifest_root, requests[0]);
  const VerifiedManifestEnvelope contract =
      read_exact_manifest(manifest_root, requests[1]);
  const VerifiedManifestEnvelope build =
      read_exact_manifest(manifest_root, requests[2]);
  if (compiler_build_contract_digest(build) !=
      compiler_contract_digest.value()) {
    fail("canonical package compiler-build manifest selects another "
         "compiler contract");
  }

  return PackageAdmissionProfile(
      environment.digest,
      contract.digest, build.digest);
}

}  // namespace manyforge::deployment::detail
