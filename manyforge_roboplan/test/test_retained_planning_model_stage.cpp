#include "manyforge_roboplan/retained_planning_model_stage.hpp"

#include <sys/stat.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

constexpr std::string_view kMeshBytes =
    "solid triangle\n"
    "facet normal 0 0 1\n"
    "outer loop\n"
    "vertex 0 0 0\n"
    "vertex 1 0 0\n"
    "vertex 0 1 0\n"
    "endloop\n"
    "endfacet\n"
    "endsolid triangle\n";
constexpr std::string_view kMeshDigest =
    "sha256:0cb2709a4b8b7e48eabb694fd0eff87cb41196efe978e75128b6164bb29e8ac0";
constexpr std::string_view kMeshStoreKey =
    "sha256/0cb2709a4b8b7e48eabb694fd0eff87cb41196efe978e75128b6164bb29e8ac0";

[[nodiscard]] std::string revision_scoped_uri(
    std::string_view asset_id, char revision_fill,
    char source_fill) {
  return "asset://" + std::string(asset_id) +
         "/mf1/revision/" + std::string(64U, revision_fill) + "/" +
         std::string(64U, source_fill) + "/base.stl";
}

class RetainedPlanningModelStageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::filesystem::path pattern =
        std::filesystem::temp_directory_path() /
        "manyforge-retained-planning-test-XXXXXX";
    std::string owned_pattern = pattern.string();
    char* created = ::mkdtemp(owned_pattern.data());
    ASSERT_NE(created, nullptr);
    parent_ = created;
  }

  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(parent_, ignored);
  }

  [[nodiscard]] static RetainedPlanningResourceInput resource(
      std::string logical_uri =
          "asset://arm/meshes/collision/base.stl") {
    return {
        std::move(logical_uri),
        "model/stl",
        static_cast<std::uint64_t>(kMeshBytes.size()),
        std::string(kMeshDigest),
        std::string(kMeshStoreKey),
        std::string(kMeshBytes),
    };
  }

  [[nodiscard]] static RetainedPlanningModelInput model(
      std::vector<RetainedPlanningResourceInput> resources = {
          resource()}) {
    RetainedPlanningModelInput input;
    input.urdf_bytes =
        "<robot name=\"retained\"><link name=\"base\"><collision><geometry>"
        "<mesh filename=\"asset://arm/meshes/collision/base.stl\"/>"
        "</geometry></collision></link></robot>";
    input.srdf_bytes = "<robot name=\"retained\"></robot>";
    input.resources = std::move(resources);
    return input;
  }

  [[nodiscard]] std::size_t child_count() const {
    return static_cast<std::size_t>(
        std::distance(std::filesystem::directory_iterator(parent_),
                      std::filesystem::directory_iterator()));
  }

  std::filesystem::path parent_;
};

TEST_F(RetainedPlanningModelStageTest,
       StagesExactPrivateTreeAndRemovesItAtLeaseEnd) {
  std::filesystem::path first_root;
  std::filesystem::path second_root;
  {
    auto first = RetainedPlanningModelStage::create(model(), parent_);
    auto second = RetainedPlanningModelStage::create(model(), parent_);
    first_root = first.staging_root();
    second_root = second.staging_root();

    EXPECT_NE(first_root, second_root);
    ASSERT_EQ(first.package_paths().size(), 1U);
    EXPECT_EQ(first.package_paths().front(), first_root);
    EXPECT_EQ(first.loader_urdf_bytes().find("asset://"),
              std::string_view::npos);
    EXPECT_NE(first.loader_urdf_bytes().find(
                  "package://arm/meshes/collision/base.stl"),
              std::string_view::npos);
    EXPECT_EQ(first.loader_urdf_bytes(), second.loader_urdf_bytes());
    EXPECT_EQ(first.srdf_bytes(), "<robot name=\"retained\"></robot>");

    EXPECT_EQ(first.loader_urdf_path(),
              first_root / "retained-model.urdf");
    EXPECT_EQ(first.srdf_path(), first_root / "retained-model.srdf");
    for (const auto& [path, expected] :
         std::array<std::pair<std::filesystem::path, std::string_view>, 2U>{
             std::pair{first.loader_urdf_path(),
                       first.loader_urdf_bytes()},
             std::pair{first.srdf_path(), first.srdf_bytes()}}) {
      struct stat model_status {};
      ASSERT_EQ(::stat(path.c_str(), &model_status), 0);
      EXPECT_TRUE(S_ISREG(model_status.st_mode));
      EXPECT_EQ(model_status.st_mode & 0777, 0400);
      EXPECT_EQ(model_status.st_nlink, 1);
      std::ifstream model_stream(path, std::ios::binary);
      ASSERT_TRUE(model_stream);
      EXPECT_EQ(std::string(std::istreambuf_iterator<char>(model_stream),
                            std::istreambuf_iterator<char>()),
                expected);
    }

    struct stat root_status {};
    ASSERT_EQ(::stat(first_root.c_str(), &root_status), 0);
    EXPECT_EQ(root_status.st_mode & 0777, 0700);

    const auto staged_mesh =
        first_root / "arm/meshes/collision/base.stl";
    struct stat mesh_status {};
    ASSERT_EQ(::stat(staged_mesh.c_str(), &mesh_status), 0);
    EXPECT_TRUE(S_ISREG(mesh_status.st_mode));
    EXPECT_EQ(mesh_status.st_mode & 0777, 0400);
    EXPECT_EQ(mesh_status.st_nlink, 1);

    std::ifstream stream(staged_mesh, std::ios::binary);
    ASSERT_TRUE(stream);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(stream),
                          std::istreambuf_iterator<char>()),
              kMeshBytes);
    EXPECT_TRUE(std::filesystem::is_regular_file(
        first_root / ".manyforge-ready"));

    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(first_root)) {
      EXPECT_FALSE(entry.path().filename().string().starts_with(
          ".manyforge-partial-"));
    }
  }

  EXPECT_FALSE(std::filesystem::exists(first_root));
  EXPECT_FALSE(std::filesystem::exists(second_root));
}

TEST_F(RetainedPlanningModelStageTest,
       PlannedGetterBridgeDoesNotRequireDeploymentTypes) {
  struct FakeSize final {
    std::uint64_t count;
    [[nodiscard]] std::uint64_t value() const { return count; }
  };
  struct FakeResource final {
    RetainedPlanningResourceInput value;
    [[nodiscard]] const std::string& logical_uri() const {
      return value.logical_uri;
    }
    [[nodiscard]] const std::string& media_type() const {
      return value.media_type;
    }
    [[nodiscard]] FakeSize byte_size() const {
      return {value.byte_size};
    }
    [[nodiscard]] const std::string& content_digest() const {
      return value.content_digest;
    }
    [[nodiscard]] const std::string& content_addressed_store_key() const {
      return value.content_addressed_store_key;
    }
    [[nodiscard]] const std::string& bytes() const { return value.bytes; }
  };
  struct FakeModel final {
    RetainedPlanningModelInput value;
    std::vector<FakeResource> retained_resources;
    [[nodiscard]] const std::string& urdf_bytes() const {
      return value.urdf_bytes;
    }
    [[nodiscard]] const std::string& srdf_bytes() const {
      return value.srdf_bytes;
    }
    [[nodiscard]] const std::vector<FakeResource>& resources() const {
      return retained_resources;
    }
  };

  FakeModel fake;
  fake.value = model();
  fake.retained_resources.push_back({fake.value.resources.front()});
  auto stage =
      RetainedPlanningModelStage::create_verified(fake, parent_);
  EXPECT_TRUE(std::filesystem::is_regular_file(
      stage.staging_root() / "arm/meshes/collision/base.stl"));
}

TEST_F(RetainedPlanningModelStageTest,
       RepeatedExactAssetReferenceStagesOneRevisionScopedResource) {
  const std::string uri =
      revision_scoped_uri("arm", 'a', 'b');
  RetainedPlanningModelInput input;
  input.urdf_bytes =
      "<robot name=\"retained\"><link name=\"first\"><visual><geometry>"
      "<mesh filename=\"" +
      uri +
      "\"/></geometry></visual></link><link name=\"second\"><collision>"
      "<geometry><mesh filename=\"" +
      uri +
      "\"/></geometry></collision></link></robot>";
  input.srdf_bytes = "<robot name=\"retained\"></robot>";
  input.resources = {resource(uri)};

  auto stage =
      RetainedPlanningModelStage::create(std::move(input), parent_);
  const std::string package_uri =
      "package://" + uri.substr(std::string_view("asset://").size());
  const auto first =
      stage.loader_urdf_bytes().find(package_uri);
  ASSERT_NE(first, std::string_view::npos);
  EXPECT_NE(
      stage.loader_urdf_bytes().find(
          package_uri, first + package_uri.size()),
      std::string_view::npos);
  EXPECT_TRUE(std::filesystem::is_regular_file(
      stage.staging_root() /
      uri.substr(std::string_view("asset://").size())));
}

TEST_F(RetainedPlanningModelStageTest,
       SamePathAcrossAssetsAndRevisionsStagesIndependentResources) {
  const std::string first_uri =
      revision_scoped_uri("arm", 'a', 'c');
  const std::string second_uri =
      revision_scoped_uri("arm", 'b', 'c');
  const std::string third_uri =
      revision_scoped_uri("other_arm", 'a', 'c');
  RetainedPlanningModelInput input;
  input.urdf_bytes =
      "<robot name=\"retained\"><link name=\"first\"><visual><geometry>"
      "<mesh filename=\"" +
      first_uri +
      "\"/></geometry></visual></link><link name=\"second\"><visual>"
      "<geometry><mesh filename=\"" +
      second_uri +
      "\"/></geometry></visual></link><link name=\"third\"><collision>"
      "<geometry><mesh filename=\"" +
      third_uri +
      "\"/></geometry></collision></link></robot>";
  input.srdf_bytes = "<robot name=\"retained\"></robot>";
  input.resources = {
      resource(first_uri), resource(second_uri), resource(third_uri)};

  auto stage =
      RetainedPlanningModelStage::create(std::move(input), parent_);
  for (const auto& uri : {first_uri, second_uri, third_uri}) {
    EXPECT_TRUE(std::filesystem::is_regular_file(
        stage.staging_root() /
        uri.substr(std::string_view("asset://").size())));
    EXPECT_NE(
        stage.loader_urdf_bytes().find(
            "package://" +
            uri.substr(std::string_view("asset://").size())),
        std::string_view::npos);
  }
}

TEST_F(RetainedPlanningModelStageTest,
       ConcurrentStagesRemainPrivateAndIndependent) {
  auto first_future = std::async(std::launch::async, [this] {
    return RetainedPlanningModelStage::create(model(), parent_);
  });
  auto second_future = std::async(std::launch::async, [this] {
    return RetainedPlanningModelStage::create(model(), parent_);
  });

  auto first = first_future.get();
  auto second = second_future.get();
  EXPECT_NE(first.staging_root(), second.staging_root());
  EXPECT_EQ(first.loader_urdf_bytes(), second.loader_urdf_bytes());
  EXPECT_TRUE(std::filesystem::is_regular_file(
      first.staging_root() / "arm/meshes/collision/base.stl"));
  EXPECT_TRUE(std::filesystem::is_regular_file(
      second.staging_root() / "arm/meshes/collision/base.stl"));
}

TEST_F(RetainedPlanningModelStageTest,
       RejectsUnsafeOrNonAssetLogicalUrisBeforeCreatingAStage) {
  const std::vector<std::string> invalid_uris{
      "package://arm/mesh.stl",
      "asset://arm",
      "asset://arm/",
      "asset://arm/../mesh.stl",
      "asset://arm/./mesh.stl",
      "asset://arm/mesh%2estl",
      "asset://arm/mesh.stl?variant=1",
      "asset://arm/mesh.stl#fragment",
      "asset://arm\\mesh.stl",
  };
  for (const auto& invalid_uri : invalid_uris) {
    SCOPED_TRACE(invalid_uri);
    EXPECT_THROW(
        (void)RetainedPlanningModelStage::create(
            model({resource(invalid_uri)}), parent_),
        RetainedPlanningStageError);
    EXPECT_EQ(child_count(), 0U);
  }
}

TEST_F(RetainedPlanningModelStageTest,
       RejectsDuplicateAndFileDirectoryResourceCollisions) {
  EXPECT_THROW(
      (void)RetainedPlanningModelStage::create(
          model({resource(), resource()}), parent_),
      RetainedPlanningStageError);

  EXPECT_THROW(
      (void)RetainedPlanningModelStage::create(
          model({resource("asset://arm/path"),
                 resource("asset://arm/path/mesh.stl")}),
          parent_),
      RetainedPlanningStageError);
  EXPECT_EQ(child_count(), 0U);
}

TEST_F(RetainedPlanningModelStageTest,
       RechecksSizeDigestStoreKeyAndMediaType) {
  auto wrong_size = resource();
  ++wrong_size.byte_size;
  EXPECT_THROW(
      (void)RetainedPlanningModelStage::create(
          model({std::move(wrong_size)}), parent_),
      RetainedPlanningStageError);

  auto wrong_digest = resource();
  wrong_digest.content_digest.back() = '1';
  EXPECT_THROW(
      (void)RetainedPlanningModelStage::create(
          model({std::move(wrong_digest)}), parent_),
      RetainedPlanningStageError);

  auto wrong_store_key = resource();
  wrong_store_key.content_addressed_store_key.back() = '1';
  EXPECT_THROW(
      (void)RetainedPlanningModelStage::create(
          model({std::move(wrong_store_key)}), parent_),
      RetainedPlanningStageError);

  auto wrong_media_type = resource();
  wrong_media_type.media_type = "application/octet-stream";
  EXPECT_THROW(
      (void)RetainedPlanningModelStage::create(
          model({std::move(wrong_media_type)}), parent_),
      RetainedPlanningStageError);
  EXPECT_EQ(child_count(), 0U);
}

TEST_F(RetainedPlanningModelStageTest,
       RequiresVerifiedReferencesAndForbidsAmbientLoaderFallbacks) {
  auto missing = model();
  missing.resources.clear();
  EXPECT_THROW(
      (void)RetainedPlanningModelStage::create(
          std::move(missing), parent_),
      RetainedPlanningStageError);

  {
    auto closure =
        model({resource(), resource("asset://arm/unused.stl")});
    auto stage = RetainedPlanningModelStage::create(
        std::move(closure), parent_);
    EXPECT_TRUE(std::filesystem::is_regular_file(
        stage.staging_root() / "arm/unused.stl"));
  }

  auto package_uri = model();
  package_uri.urdf_bytes =
      "<robot name=\"retained\"><link name=\"base\"><visual><geometry>"
      "<mesh filename=\"package://ambient/mesh.stl\"/>"
      "</geometry></visual></link></robot>";
  EXPECT_THROW(
      (void)RetainedPlanningModelStage::create(
          std::move(package_uri), parent_),
      RetainedPlanningStageError);

  auto srdf_lookup = model();
  srdf_lookup.srdf_bytes =
      "<robot name=\"retained\">asset://arm/mesh.stl</robot>";
  EXPECT_THROW(
      (void)RetainedPlanningModelStage::create(
          std::move(srdf_lookup), parent_),
      RetainedPlanningStageError);
  EXPECT_EQ(child_count(), 0U);
}

TEST_F(RetainedPlanningModelStageTest,
       EnforcesCallerLimitsAndHardProductionCeilings) {
  RetainedPlanningStageLimits count_limit;
  count_limit.maximum_resource_count = 1U;
  EXPECT_THROW(
      (void)RetainedPlanningModelStage::create(
          model({resource(), resource("asset://arm/other.stl")}),
          parent_, count_limit),
      RetainedPlanningStageError);

  RetainedPlanningStageLimits byte_limit;
  byte_limit.maximum_resource_bytes =
      static_cast<std::uint64_t>(kMeshBytes.size() - 1U);
  EXPECT_THROW(
      (void)RetainedPlanningModelStage::create(
          model(), parent_, byte_limit),
      RetainedPlanningStageError);

  RetainedPlanningStageLimits hard_limit;
  hard_limit.maximum_total_bytes =
      RetainedPlanningModelStage::kHardMaximumTotalBytes + 1U;
  EXPECT_THROW(
      (void)RetainedPlanningModelStage::create(
          model(), parent_, hard_limit),
      RetainedPlanningStageError);
  EXPECT_EQ(child_count(), 0U);
}

TEST_F(RetainedPlanningModelStageTest,
       RejectsSymlinkParentAndDoesNotAdoptCrashResidue) {
  const auto real_parent = parent_ / "real";
  const auto symlink_parent = parent_ / "link";
  ASSERT_TRUE(std::filesystem::create_directory(real_parent));
  ASSERT_NO_THROW(
      std::filesystem::create_directory_symlink(real_parent, symlink_parent));
  EXPECT_THROW(
      (void)RetainedPlanningModelStage::create(
          model(), symlink_parent),
      RetainedPlanningStageError);

  const auto residue =
      parent_ / "manyforge-retained-planning-crash-residue";
  ASSERT_TRUE(std::filesystem::create_directory(residue));
  const auto sentinel = residue / "sentinel";
  {
    std::ofstream stream(sentinel);
    ASSERT_TRUE(stream);
    stream << "owned by an earlier process";
  }

  {
    auto stage = RetainedPlanningModelStage::create(model(), parent_);
    EXPECT_NE(stage.staging_root(), residue);
    EXPECT_TRUE(std::filesystem::is_regular_file(sentinel));
  }
  EXPECT_TRUE(std::filesystem::is_regular_file(sentinel));
}

}  // namespace
}  // namespace manyforge::roboplan_backend
