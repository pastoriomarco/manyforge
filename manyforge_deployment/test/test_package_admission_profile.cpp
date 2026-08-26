#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "build_manifest_contract_internal.hpp"
#include "build_manifest_internal.hpp"
#include "content_identity_internal.hpp"
#include "manyforge_deployment/build_manifest.hpp"
#include "package_admission_profile_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    std::string pattern =
        (std::filesystem::temp_directory_path() /
         "manyforge-package-profile-XXXXXX")
            .string();
    std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
    mutable_pattern.push_back('\0');
    const char* created = ::mkdtemp(mutable_pattern.data());
    if (created == nullptr) {
      throw std::runtime_error("could not create test directory");
    }
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

[[nodiscard]] CanonicalValue replace_member(
    const CanonicalValue& value, std::string_view name,
    CanonicalValue replacement) {
  CanonicalValue::Object result = value.as_object();
  const auto found = std::find_if(
      result.begin(), result.end(),
      [name](const auto& row) { return row.first == name; });
  if (found == result.end()) {
    throw std::logic_error("test replacement target is absent");
  }
  found->second = std::move(replacement);
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue reidentify(
    CanonicalValue candidate, std::string_view own_field,
    std::string_view identity_domain, std::string_view api_version) {
  candidate = replace_member(
      candidate, own_field,
      CanonicalValue(std::string(kZeroDigest)));
  const std::string digest = compute_content_identity(
      candidate,
      ContentIdentitySpec{
          .identity_domain = std::string(identity_domain),
          .api_version = std::string(api_version),
          .own_field = std::string(own_field),
      });
  return replace_member(candidate, own_field, CanonicalValue(digest));
}

[[nodiscard]] CanonicalValue reidentify_build(CanonicalValue candidate) {
  return reidentify(std::move(candidate), "compiler_build_digest",
                    kCompilerBuildIdentityDomain,
                    kCompilerBuildManifestApiVersion);
}

[[nodiscard]] const std::string& text_member(
    const CanonicalValue& value, std::string_view name) {
  const CanonicalValue* member = value.find_object_member(name);
  if (member == nullptr) {
    throw std::logic_error("test manifest member is absent");
  }
  return std::get<std::string>(member->storage());
}

[[nodiscard]] std::filesystem::path manifest_path(
    const std::filesystem::path& root, BuildManifestKind kind,
    std::string_view digest) {
  return root / "build-manifests" / std::string(to_string(kind)) /
         (std::string(digest.substr(std::string_view("sha256:").size())) +
          ".json");
}

void write_manifest(const std::filesystem::path& root,
                    BuildManifestKind kind, std::string_view digest,
                    std::string_view bytes) {
  const std::filesystem::path path = manifest_path(root, kind, digest);
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.good());
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.flush();
  ASSERT_TRUE(output.good());
}

[[nodiscard]] BuildQualifiedRetentionContext write_current_profile(
    const std::filesystem::path& root) {
  BuildQualifiedRetentionContext current =
      BuildQualifiedRetentionAuthority::qualify();
  for (const auto& manifest : current.manifests()) {
    write_manifest(root, manifest->key().manifest_kind,
                   manifest->key().content_digest.value(),
                   manifest->canonical_bytes());
  }
  return current;
}

TEST(PackageAdmissionProfileTest,
     AcceptsExactSelfVerifiedProducerClosure) {
  TemporaryDirectory package;
  const auto current = write_current_profile(package.path());

  const auto profile = verify_package_admission_profile(
      package.path(), current.node_environment().environment_digest,
      current.compiler_contract_digest(), current.compiler_build_digest());

  EXPECT_EQ(profile.environment_digest(),
            current.node_environment().environment_digest);
  EXPECT_EQ(profile.compiler_contract_digest(),
            current.compiler_contract_digest());
  EXPECT_EQ(profile.compiler_build_digest(), current.compiler_build_digest());
}

TEST(PackageAdmissionProfileTest,
     AcceptsSelfConsistentDifferentProducerBuild) {
  TemporaryDirectory package;
  const auto current = write_current_profile(package.path());
  const CanonicalValue historical = reidentify_build(replace_member(
      BuildManifestTestAccess::current_candidate(
          BuildManifestKind::kCompilerBuild),
      "build_profile", CanonicalValue("historical-compatible-build")));
  const std::string historical_digest =
      text_member(historical, "compiler_build_digest");
  ASSERT_NE(historical_digest, current.compiler_build_digest().value());
  write_manifest(package.path(), BuildManifestKind::kCompilerBuild,
                 historical_digest, canonical_json(historical));

  const auto profile = verify_package_admission_profile(
      package.path(), current.node_environment().environment_digest,
      current.compiler_contract_digest(),
      core::ContentDigest(historical_digest));

  EXPECT_EQ(profile.compiler_build_digest().value(), historical_digest);
  EXPECT_EQ(profile.compiler_contract_digest(),
            current.compiler_contract_digest());
}

TEST(PackageAdmissionProfileTest,
     AcceptsSelfConsistentDifferentProducerEnvironment) {
  TemporaryDirectory package;
  const auto current = write_current_profile(package.path());
  const CanonicalValue current_environment =
      BuildManifestTestAccess::current_candidate(
          BuildManifestKind::kNodeEnvironment);
  const CanonicalValue* current_catalog =
      current_environment.find_object_member("catalog");
  ASSERT_NE(current_catalog, nullptr);
  const CanonicalValue historical_catalog = replace_member(
      *current_catalog, "catalog_digest",
      CanonicalValue(
          "sha256:1111111111111111111111111111111111111111111111111111111111111111"));
  const CanonicalValue historical = reidentify(
      replace_member(current_environment, "catalog",
                     historical_catalog),
      "environment_digest", kNodeEnvironmentIdentityDomain,
      kNodeEnvironmentManifestApiVersion);
  const std::string historical_digest =
      text_member(historical, "environment_digest");
  ASSERT_NE(historical_digest,
            current.node_environment().environment_digest.value());
  write_manifest(package.path(), BuildManifestKind::kNodeEnvironment,
                 historical_digest, canonical_json(historical));

  const auto profile = verify_package_admission_profile(
      package.path(), core::ContentDigest(historical_digest),
      current.compiler_contract_digest(), current.compiler_build_digest());

  EXPECT_EQ(profile.environment_digest().value(), historical_digest);
}

TEST(PackageAdmissionProfileTest,
     MissingRootFailsWithoutCreatingOrRepairingIt) {
  TemporaryDirectory parent;
  const std::filesystem::path absent = parent.path() / "absent";
  const auto current = BuildQualifiedRetentionAuthority::qualify();

  EXPECT_THROW(
      (void)verify_package_admission_profile(
          absent, current.node_environment().environment_digest,
          current.compiler_contract_digest(), current.compiler_build_digest()),
      BuildManifestError);
  EXPECT_FALSE(std::filesystem::exists(absent));
}

TEST(PackageAdmissionProfileTest, RejectsOneByteCorruptionWithoutRepair) {
  TemporaryDirectory package;
  const auto current = write_current_profile(package.path());
  const auto path = manifest_path(
      package.path(), BuildManifestKind::kNodeEnvironment,
      current.node_environment().environment_digest.value());
  {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file.good());
    char first = '\0';
    file.read(&first, 1);
    ASSERT_TRUE(file.good());
    first = first == '{' ? '[' : '{';
    file.seekp(0);
    file.write(&first, 1);
  }
  const auto size_before = std::filesystem::file_size(path);

  EXPECT_THROW(
      (void)verify_package_admission_profile(
          package.path(), current.node_environment().environment_digest,
          current.compiler_contract_digest(), current.compiler_build_digest()),
      BuildManifestError);
  EXPECT_EQ(std::filesystem::file_size(path), size_before);
}

TEST(PackageAdmissionProfileTest, RejectsCrossSplicedManifestBytes) {
  TemporaryDirectory package;
  const auto current = write_current_profile(package.path());
  const auto contract = reload_self_verified_build_manifest(
      BuildManifestKind::kCompilerContract,
      canonical_json(BuildManifestTestAccess::current_candidate(
          BuildManifestKind::kCompilerContract)));
  write_manifest(package.path(), BuildManifestKind::kCompilerBuild,
                 current.compiler_build_digest().value(),
                 contract->canonical_bytes());

  EXPECT_THROW(
      (void)verify_package_admission_profile(
          package.path(), current.node_environment().environment_digest,
          current.compiler_contract_digest(), current.compiler_build_digest()),
      BuildManifestError);
}

TEST(PackageAdmissionProfileTest,
     RejectsCompilerBuildBoundToAnotherContract) {
  TemporaryDirectory package;
  const auto current = write_current_profile(package.path());
  const CanonicalValue cross_bound = reidentify_build(replace_member(
      BuildManifestTestAccess::current_candidate(
          BuildManifestKind::kCompilerBuild),
      "compiler_contract_digest",
      CanonicalValue("sha256:1111111111111111111111111111111111111111111111111111111111111111")));
  const std::string build_digest =
      text_member(cross_bound, "compiler_build_digest");
  write_manifest(package.path(), BuildManifestKind::kCompilerBuild,
                 build_digest, canonical_json(cross_bound));

  EXPECT_THROW(
      (void)verify_package_admission_profile(
          package.path(), current.node_environment().environment_digest,
          current.compiler_contract_digest(), core::ContentDigest(build_digest)),
      BuildManifestError);
}

}  // namespace
}  // namespace manyforge::deployment::detail
