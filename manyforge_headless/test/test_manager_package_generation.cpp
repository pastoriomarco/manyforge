#include <manyforge_headless/manager_package_generation.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <cstdlib>
#include <string>
#include <stdexcept>
#include <vector>

#include <manyforge_core/runtime_contract_manifest_codec.hpp>

namespace manyforge::headless {
namespace {

class TemporaryRoot final {
public:
  TemporaryRoot() {
    path_ = std::filesystem::temp_directory_path() /
            std::filesystem::path("manyforge-manager-package-generation-XXXXXX");
    auto spelling = path_.string();
    spelling.push_back('\0');
    if (::mkdtemp(spelling.data()) == nullptr) {
      throw std::runtime_error("manager package generation mkdtemp failed");
    }
    path_ = spelling.c_str();
  }
  ~TemporaryRoot() { std::filesystem::remove_all(path_); }
  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

core::RuntimeContractManifestSetDTO Manifest(std::string family) {
  auto offer = core::MakeRuntimeContractOffer(
      core::RuntimeImplementationFamilyId(std::move(family)),
      {core::ApiContractId("manyforge.test.api.v1")}, {});
  EXPECT_TRUE(offer.ok());
  auto manifest =
      core::MakeRuntimeContractManifestSet({std::move(offer.value())});
  EXPECT_TRUE(manifest.ok());
  return std::move(manifest.value());
}

TEST(ManagerPackageGeneration, UnchangedRootAndManifestRemainCurrent) {
  TemporaryRoot root;
  const auto manifest = Manifest("manyforge.test.runtime");
  ManagerPackageGeneration generation(root.path(), manifest);
  EXPECT_EQ(generation.manifest_set(), manifest);
  const auto descriptor_root = generation.descriptor_bound_root();
  EXPECT_TRUE(std::filesystem::is_directory(descriptor_root));
  EXPECT_NE(descriptor_root.generic_string().find("/proc/self/fd/"),
            std::string::npos);
  EXPECT_NO_THROW(generation.RequireCurrent(manifest));
  EXPECT_NO_THROW(generation.RequireCurrent(manifest));
}

TEST(ManagerPackageGeneration, RootReplacementLatchesRestartRequired) {
  TemporaryRoot holder;
  const auto selected = holder.path() / "selected";
  const auto moved = holder.path() / "moved";
  ASSERT_TRUE(std::filesystem::create_directory(selected));
  const auto manifest = Manifest("manyforge.test.runtime");
  ManagerPackageGeneration generation(selected, manifest);
  std::filesystem::rename(selected, moved);
  ASSERT_TRUE(std::filesystem::create_directory(selected));

  for (int attempt = 0; attempt < 2; ++attempt) {
    try {
      generation.RequireCurrent(manifest);
      FAIL() << "replacement must require a full stack restart";
    } catch (const ManagerPackageGenerationRestartRequired &error) {
      EXPECT_EQ(error.error().code, "full_stack_restart_required");
      EXPECT_EQ(error.error().details.at("reason"),
                "snapshot_integrity_lost");
      EXPECT_EQ(error.error().details.at("retryable"), "false");
      EXPECT_EQ(error.error().details.at("required_action"),
                "complete_stack_stop_start");
    }
  }
}

TEST(ManagerPackageGeneration, ManifestMismatchLatchesObservedIdentity) {
  TemporaryRoot root;
  const auto expected = Manifest("manyforge.test.runtime");
  const auto observed = Manifest("manyforge.test.other-runtime");
  ManagerPackageGeneration generation(root.path(), expected);
  try {
    generation.RequireCurrent(observed);
    FAIL() << "manifest mismatch must require a full stack restart";
  } catch (const ManagerPackageGenerationRestartRequired &error) {
    EXPECT_EQ(error.error().details.at("reason"), "manifest_set_mismatch");
    EXPECT_EQ(error.error().details.at("expected_manifest_set_digest"),
              expected.manifest_set_digest.value());
    EXPECT_EQ(error.error().details.at("observed_manifest_set_digest"),
              observed.manifest_set_digest.value());
  }
  EXPECT_THROW(generation.RequireCurrent(expected),
               ManagerPackageGenerationRestartRequired);
}

} // namespace
} // namespace manyforge::headless
