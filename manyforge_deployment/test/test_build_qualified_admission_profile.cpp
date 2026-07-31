#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "build_qualified_admission_profile_generated.hpp"
#include "build_qualified_admission_profile_internal.hpp"
#include "manyforge_deployment/build_manifest.hpp"

namespace manyforge::deployment::detail {
namespace {

namespace generated = generated_build_qualified_admission_profile;

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    std::string pattern =
        (std::filesystem::temp_directory_path() /
         "manyforge-admission-profile-XXXXXX")
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

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

struct ManifestFixture final {
  BuildManifestKind kind;
  std::string_view digest;
  std::span<const std::uint8_t> bytes;
};

constexpr std::array<ManifestFixture, 3U> kManifests{{
    {
        BuildManifestKind::kNodeEnvironment,
        generated::kNodeEnvironmentManifestDigest,
        generated::kNodeEnvironmentManifestBytes,
    },
    {
        BuildManifestKind::kCompilerContract,
        generated::kCompilerContractManifestDigest,
        generated::kCompilerContractManifestBytes,
    },
    {
        BuildManifestKind::kCompilerBuild,
        generated::kCompilerBuildManifestDigest,
        generated::kCompilerBuildManifestBytes,
    },
}};

[[nodiscard]] std::filesystem::path manifest_path(
    const std::filesystem::path& root,
    const ManifestFixture& manifest) {
  return root / "build-manifests" /
         std::string(to_string(manifest.kind)) /
         (std::string(manifest.digest.substr(
              std::string_view("sha256:").size())) +
          ".json");
}

void write_manifest(const std::filesystem::path& root,
                    const ManifestFixture& manifest,
                    std::span<const std::uint8_t> bytes) {
  const std::filesystem::path path = manifest_path(root, manifest);
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.good());
  output.write(
      reinterpret_cast<const char*>(bytes.data()),
      static_cast<std::streamsize>(bytes.size()));
  output.flush();
  ASSERT_TRUE(output.good());
}

void write_exact_profile(const std::filesystem::path& root) {
  for (const ManifestFixture& manifest : kManifests) {
    write_manifest(root, manifest, manifest.bytes);
  }
}

TEST(BuildQualifiedAdmissionProfileTest,
     AcceptsOnlyExactBuildGeneratedClosure) {
  TemporaryDirectory package;
  write_exact_profile(package.path());

  const auto authority =
      verify_build_qualified_admission_profile(package.path());
  EXPECT_EQ(authority.node_environment().catalog_id,
            generated::kRetainedCatalogId);
  EXPECT_EQ(authority.node_environment().content_version,
            generated::kRetainedCatalogContentVersion);
  EXPECT_EQ(authority.node_environment().catalog_digest.value(),
            generated::kRetainedCatalogDigest);
  EXPECT_EQ(authority.node_environment().environment_digest.value(),
            generated::kRetainedEnvironmentDigest);
  EXPECT_EQ(authority.compiler_contract_digest().value(),
            generated::kCompilerContractManifestDigest);
  EXPECT_EQ(authority.compiler_build_digest().value(),
            generated::kCompilerBuildManifestDigest);
}

TEST(BuildQualifiedAdmissionProfileTest,
     MissingRootFailsWithoutCreatingOrRepairingIt) {
  TemporaryDirectory parent;
  const std::filesystem::path absent = parent.path() / "absent";

  EXPECT_THROW(
      (void)verify_build_qualified_admission_profile(absent),
      BuildManifestError);
  EXPECT_FALSE(std::filesystem::exists(absent));
}

TEST(BuildQualifiedAdmissionProfileTest,
     RejectsOneByteCorruptionWithoutRepairingIt) {
  TemporaryDirectory package;
  write_exact_profile(package.path());
  const std::filesystem::path node_path =
      manifest_path(package.path(), kManifests.front());
  {
    std::fstream file(
        node_path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file.good());
    char first = '\0';
    file.read(&first, 1);
    ASSERT_TRUE(file.good());
    first = first == '{' ? '[' : '{';
    file.seekp(0);
    file.write(&first, 1);
    file.flush();
    ASSERT_TRUE(file.good());
  }
  const auto size_before = std::filesystem::file_size(node_path);

  EXPECT_THROW(
      (void)verify_build_qualified_admission_profile(package.path()),
      BuildManifestError);
  EXPECT_EQ(std::filesystem::file_size(node_path), size_before);
}

TEST(BuildQualifiedAdmissionProfileTest,
     RejectsCrossSplicedManifestBytes) {
  TemporaryDirectory package;
  write_exact_profile(package.path());
  write_manifest(
      package.path(), kManifests[2], kManifests[1].bytes);

  EXPECT_THROW(
      (void)verify_build_qualified_admission_profile(package.path()),
      BuildManifestError);
}

}  // namespace
}  // namespace manyforge::deployment::detail
