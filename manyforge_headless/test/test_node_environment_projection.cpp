#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>

#include "manyforge_headless/node_environment_projection.hpp"

namespace manyforge::headless {
namespace {

namespace content_identity = manyforge::content_identity;
namespace deployment = manyforge::deployment;

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "manyforge-node-environment-XXXXXX")
                       .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto *created = ::mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("node-environment test mkdtemp failed");
    }
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::vector<char> buffer_;
  std::filesystem::path path_;
};

deployment::CanonicalEmptyRetainedBundleAdmission
LoadAdmission(const std::filesystem::path &package_root =
                  MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT) {
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource source(
      package_root);
  return source.load_verified();
}

std::string Read(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot read test input " + path.string());
  }
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

void Write(const std::filesystem::path &path, const std::string &bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream ||
      !stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
    throw std::runtime_error("cannot write test input " + path.string());
  }
}

void ReplaceOnce(std::string &bytes, const std::string &before,
                 const std::string &after) {
  const auto offset = bytes.find(before);
  if (offset == std::string::npos) {
    throw std::runtime_error("test mutation target is absent");
  }
  bytes.replace(offset, before.size(), after);
}

std::size_t Count(const std::string &bytes, const std::string &needle) {
  std::size_t result = 0U;
  for (std::size_t offset = 0U;
       (offset = bytes.find(needle, offset)) != std::string::npos;
       offset += needle.size()) {
    ++result;
  }
  return result;
}

std::filesystem::path CopyPackage(const TemporaryDirectory &directory) {
  const auto copy = directory.path() / "package";
  std::filesystem::copy(MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT,
                        copy, std::filesystem::copy_options::recursive);
  return copy;
}

TEST(NodeEnvironmentProjection,
     PublishesTheExactActiveProductAndH0DynamicAvailability) {
  const auto admission = LoadAdmission();
  const ActiveNodeEnvironmentProjection projection(
      MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT,
      MANYFORGE_HEADLESS_TEST_NODE_CATALOG,
      MANYFORGE_HEADLESS_TEST_NODE_ENVIRONMENT_MANIFEST);

  const auto available = projection.Project(admission.retained_bundle(), true);
  const auto bytes = content_identity::canonical_json(available);
  EXPECT_NE(
      bytes.find(
          "\"schema_version\":\"manyforge.node_environment_projection.v2\""),
      std::string::npos);
  EXPECT_NE(bytes.find("\"portable_authoring_available\":true"),
            std::string::npos);
  EXPECT_NE(bytes.find("\"execution_available\":false"), std::string::npos);
  EXPECT_NE(bytes.find("\"implementation_identity_resolved\":true"),
            std::string::npos);
  EXPECT_NE(bytes.find("\"adapter_build_qualified\":true"), std::string::npos);
  EXPECT_NE(bytes.find("\"adapter_build_qualified\":false"), std::string::npos);
  EXPECT_EQ(Count(bytes, "\"adapter_build_qualified\":true"), 43U);
  EXPECT_EQ(Count(bytes, "\"adapter_build_qualified\":false"), 7U);
  EXPECT_NE(bytes.find("\"binding_available\":false"), std::string::npos);
  EXPECT_NE(bytes.find("\"owner_health_available\":false"), std::string::npos);
  EXPECT_EQ(Count(bytes, "\"owner_health_available\":true"), 11U);
  EXPECT_EQ(Count(bytes, "\"owner_health_available\":false"), 39U);
  EXPECT_EQ(bytes.find("\"cclc_node_adapter_unavailable_until_i7\""),
            std::string::npos);
  EXPECT_NE(bytes.find("\"kernel_policy_node_adapter_unavailable_until_i8\""),
            std::string::npos);
  EXPECT_EQ(bytes.find("\"simulation_node_adapter_unavailable_until_i9\""),
            std::string::npos);
  EXPECT_NE(bytes.find("\"semantic_io_node_adapter_unavailable_until_i11\""),
            std::string::npos);
  EXPECT_NE(bytes.find("\"manager_runtime_converged\":true"),
            std::string::npos);
  EXPECT_NE(bytes.find("\"restoration_gates\":[\"I6\",\"N\"]"),
            std::string::npos);
  EXPECT_NE(bytes.find(admission.retained_bundle_id().value()),
            std::string::npos);
  EXPECT_NE(bytes.find(admission.graph_revision().value()), std::string::npos);
  EXPECT_NE(bytes.find("\"package_environment\":"), std::string::npos);

  const auto unavailable = content_identity::canonical_json(
      projection.Project(admission.retained_bundle(), false));
  EXPECT_NE(unavailable.find("\"manager_runtime_converged\":false"),
            std::string::npos);
  EXPECT_NE(unavailable.find("\"manager_runtime_unavailable\""),
            std::string::npos);
}

TEST(NodeEnvironmentProjection, RejectsATamperedCatalogBeforePublication) {
  TemporaryDirectory directory;
  auto catalog = Read(MANYFORGE_HEADLESS_TEST_NODE_CATALOG);
  ReplaceOnce(catalog, "Sequence", "Sequencf");
  const auto catalog_path = directory.path() / "catalog.yaml";
  Write(catalog_path, catalog);

  EXPECT_THROW(
      ActiveNodeEnvironmentProjection(
          MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT, catalog_path,
          MANYFORGE_HEADLESS_TEST_NODE_ENVIRONMENT_MANIFEST),
      NodeEnvironmentProjectionError);
}

TEST(NodeEnvironmentProjection, RejectsASymlinkedCatalogInput) {
  TemporaryDirectory directory;
  const auto catalog_path = directory.path() / "catalog.yaml";
  std::filesystem::create_symlink(MANYFORGE_HEADLESS_TEST_NODE_CATALOG,
                                  catalog_path);

  EXPECT_THROW(
      ActiveNodeEnvironmentProjection(
          MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT, catalog_path,
          MANYFORGE_HEADLESS_TEST_NODE_ENVIRONMENT_MANIFEST),
      NodeEnvironmentProjectionError);
}

TEST(NodeEnvironmentProjection, RejectsATamperedCurrentManifest) {
  TemporaryDirectory directory;
  auto manifest = Read(MANYFORGE_HEADLESS_TEST_NODE_ENVIRONMENT_MANIFEST);
  ReplaceOnce(manifest, "manyforge_builtin", "manyforge_builtim");
  const auto manifest_path = directory.path() / "node-environment.yaml";
  Write(manifest_path, manifest);

  EXPECT_THROW(
      ActiveNodeEnvironmentProjection(
          MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT,
          MANYFORGE_HEADLESS_TEST_NODE_CATALOG, manifest_path),
      NodeEnvironmentProjectionError);
}

TEST(NodeEnvironmentProjection, RejectsATamperedRetainedManifest) {
  TemporaryDirectory directory;
  const auto package_root = CopyPackage(directory);
  const auto admission = LoadAdmission(package_root);
  const auto digest = admission.retained_bundle()
                          .value()
                          .catalogs.node_environment.environment_digest.value();
  const auto manifest_path = package_root / "build-manifests" /
                             "node_environment" / (digest.substr(7U) + ".json");
  auto manifest = Read(manifest_path);
  ReplaceOnce(manifest, "manyforge_builtin", "manyforge_builtim");
  Write(manifest_path, manifest);

  const ActiveNodeEnvironmentProjection projection(
      package_root, MANYFORGE_HEADLESS_TEST_NODE_CATALOG,
      MANYFORGE_HEADLESS_TEST_NODE_ENVIRONMENT_MANIFEST);
  EXPECT_THROW(projection.Project(admission.retained_bundle(), true),
               NodeEnvironmentProjectionError);
}

} // namespace
} // namespace manyforge::headless
