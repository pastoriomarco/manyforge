#include "build_qualified_admission_profile_internal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "build_manifest_contract_internal.hpp"
#include "build_qualified_admission_profile_generated.hpp"
#include "content_identity_internal.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/build_manifest.hpp"
#include "manyforge_deployment/canonical_value.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "posix_durability_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

namespace generated = generated_build_qualified_admission_profile;
namespace posix = posix_durability;

struct ExpectedManifest final {
  BuildManifestKind kind;
  std::span<const std::uint8_t> canonical_bytes;
  std::string_view content_digest;
  std::string_view api_version;
  std::string_view identity_domain;
  std::string_view own_digest_field;
};

[[noreturn]] void fail(std::string_view message) {
  throw BuildManifestError(std::string(message));
}

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

constexpr std::array<ExpectedManifest, 3U> kExpectedManifests{{
    {
        BuildManifestKind::kNodeEnvironment,
        generated::kNodeEnvironmentManifestBytes,
        generated::kNodeEnvironmentManifestDigest,
        kNodeEnvironmentManifestApiVersion,
        "node-environment-digest",
        "environment_digest",
    },
    {
        BuildManifestKind::kCompilerContract,
        generated::kCompilerContractManifestBytes,
        generated::kCompilerContractManifestDigest,
        kCompilerContractManifestApiVersion,
        "compiler-contract-digest",
        "compiler_contract_digest",
    },
    {
        BuildManifestKind::kCompilerBuild,
        generated::kCompilerBuildManifestBytes,
        generated::kCompilerBuildManifestDigest,
        kCompilerBuildManifestApiVersion,
        "compiler-build-digest",
        "compiler_build_digest",
    },
}};

[[nodiscard]] std::string_view require_text_member(
    const CanonicalValue& object, std::string_view member_name,
    std::string_view context) {
  if (!object.is_object()) {
    fail(std::string(context) + " must be an object");
  }
  const CanonicalValue* member = object.find_object_member(member_name);
  if (member == nullptr) {
    fail(std::string(context) + " is missing required member " +
         std::string(member_name));
  }
  const auto* text = std::get_if<std::string>(&member->storage());
  if (text == nullptr || text->empty()) {
    fail(std::string(context) + "." + std::string(member_name) +
         " must be non-empty text");
  }
  return *text;
}

[[nodiscard]] const CanonicalValue& require_object_member(
    const CanonicalValue& object, std::string_view member_name,
    std::string_view context) {
  if (!object.is_object()) {
    fail(std::string(context) + " must be an object");
  }
  const CanonicalValue* member = object.find_object_member(member_name);
  if (member == nullptr || !member->is_object()) {
    fail(std::string(context) + "." + std::string(member_name) +
         " must be an object");
  }
  return *member;
}

[[nodiscard]] std::string expected_bytes(
    const ExpectedManifest& expected) {
  return std::string(
      reinterpret_cast<const char*>(expected.canonical_bytes.data()),
      expected.canonical_bytes.size());
}

[[nodiscard]] std::filesystem::path existing_manifest_path(
    const std::filesystem::path& manifest_root,
    const ExpectedManifest& expected) {
  const std::string_view kind = to_string(expected.kind);
  if (kind.empty() || !is_sha256_digest(expected.content_digest)) {
    throw std::logic_error(
        "generated build-qualified admission profile is invalid");
  }
  const std::filesystem::path kind_root =
      manifest_root / std::string(kind);
  posix::require_existing_plain_directory(
      kind_root, kReadOnlyProfileErrors, kManifestDirectoryMessages);
  return kind_root /
         (std::string(expected.content_digest.substr(
              std::string_view("sha256:").size())) +
          ".json");
}

[[nodiscard]] CanonicalValue read_and_verify_manifest(
    const std::filesystem::path& manifest_root,
    const ExpectedManifest& expected) {
  const auto bytes = posix::read_exact_regular_file(
      existing_manifest_path(manifest_root, expected),
      kMaxArtifactEncodedBytes, false, kReadOnlyProfileErrors,
      kManifestReadMessages);
  if (!bytes.has_value()) {
    throw std::logic_error(
        "required build-qualified admission manifest disappeared");
  }

  try {
    const ParsedArtifactCandidate parsed =
        parse_strict_json_model_yaml(*bytes);
    const CanonicalValue& root = parsed.root();
    if (canonical_json(root) != *bytes) {
      fail("canonical package build manifest is not RFC 8785 canonical");
    }

    // Exact byte equality against the build-generated profile establishes the
    // complete closed manifest shape before the generic identity primitive is
    // invoked.  The digest is nevertheless recomputed below: the profile is
    // not treated as a digest-only or byte-only shortcut.
    if (*bytes != expected_bytes(expected)) {
      fail("canonical package build manifest is stale or differs from this "
           "exact reader build");
    }
    if (require_text_member(root, "api_version", "build_manifest") !=
        expected.api_version) {
      fail("canonical package build manifest has an unexpected API version");
    }
    const std::string_view declared_digest = require_text_member(
        root, expected.own_digest_field, "build_manifest");
    if (declared_digest != expected.content_digest ||
        !is_sha256_digest(declared_digest)) {
      fail("canonical package build-manifest key differs from its generated "
           "profile");
    }

    const ContentIdentitySpec identity_spec{
        std::string(expected.identity_domain),
        std::string(expected.api_version),
        std::string(expected.own_digest_field),
    };
    if (!verify_content_identity(root, identity_spec)) {
      fail("canonical package build manifest fails independent content "
           "identity verification");
    }
    return root;
  } catch (const CanonicalValueError& error) {
    fail(std::string("canonical package build manifest is invalid: ") +
         error.what());
  } catch (const ArtifactCodecError& error) {
    fail(std::string("canonical package build manifest is invalid: ") +
         error.what());
  }
}

void require_profile_text(std::string_view actual,
                          std::string_view expected,
                          std::string_view field) {
  if (actual != expected || actual.empty()) {
    fail(std::string("canonical package ") + std::string(field) +
         " differs from its generated profile");
  }
}

}  // namespace

BuildQualifiedAdmissionProfileAuthority
verify_build_qualified_admission_profile(
    const std::filesystem::path& trusted_package_root) {
  const std::filesystem::path root =
      posix::normalize_existing_plain_root(
          trusted_package_root, kReadOnlyProfileErrors,
          kTrustedRootMessages);
  const std::filesystem::path manifest_root = root / "build-manifests";
  posix::require_existing_plain_directory(
      manifest_root, kReadOnlyProfileErrors, kManifestDirectoryMessages);

  std::array<CanonicalValue, 3U> verified_roots{
      CanonicalValue(nullptr),
      CanonicalValue(nullptr),
      CanonicalValue(nullptr),
  };
  for (std::size_t index = 0U; index < kExpectedManifests.size();
       ++index) {
    verified_roots[index] =
        read_and_verify_manifest(manifest_root, kExpectedManifests[index]);
  }

  const CanonicalValue& node_environment = verified_roots[0];
  const CanonicalValue& catalog = require_object_member(
      node_environment, "catalog", "node_environment_manifest");
  require_profile_text(
      require_text_member(
          catalog, "catalog_id", "node_environment_manifest.catalog"),
      generated::kRetainedCatalogId, "retained catalog ID");
  require_profile_text(
      require_text_member(
          catalog, "content_version",
          "node_environment_manifest.catalog"),
      generated::kRetainedCatalogContentVersion,
      "retained catalog content version");
  require_profile_text(
      require_text_member(
          catalog, "catalog_digest",
          "node_environment_manifest.catalog"),
      generated::kRetainedCatalogDigest, "retained catalog digest");
  require_profile_text(
      require_text_member(
          node_environment, "environment_digest",
          "node_environment_manifest"),
      generated::kRetainedEnvironmentDigest,
      "retained environment digest");

  require_profile_text(
      require_text_member(
          verified_roots[2], "compiler_contract_digest",
          "compiler_build_manifest"),
      generated::kCompilerContractManifestDigest,
      "compiler-build contract binding");

  return BuildQualifiedAdmissionProfileAuthority(
      core::RetainedNodeEnvironmentDTO{
          std::string(generated::kRetainedCatalogId),
          std::string(generated::kRetainedCatalogContentVersion),
          core::ContentDigest(
              std::string(generated::kRetainedCatalogDigest)),
          core::ContentDigest(
              std::string(generated::kRetainedEnvironmentDigest)),
      },
      core::ContentDigest(
          std::string(generated::kCompilerContractManifestDigest)),
      core::ContentDigest(
          std::string(generated::kCompilerBuildManifestDigest)));
}

}  // namespace manyforge::deployment::detail
