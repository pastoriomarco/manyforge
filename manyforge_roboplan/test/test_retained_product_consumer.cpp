#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <Eigen/Core>
#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_core/collision/collision_world_snapshot_builder.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_roboplan/retained_planning_model_stage.hpp>
#include <roboplan/core/geometry_wrappers.hpp>
#include <roboplan/core/scene.hpp>

#include "lite6_retained_consumer_fixture.hpp"
#include "mf2_joint_name_internal.hpp"
#include "retained_bundle_internal.hpp"

namespace manyforge::roboplan_backend {
namespace {

using collision::ManyForgeCollisionEngine;
constexpr std::string_view kCanonicalCellFrame =
    "mf1_cell_e228ede37638a8d03080644d418a75c681930a4b5b3f6b0bbde2603974dab681";
constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

static_assert(!std::is_default_constructible_v<
              deployment::VerifiedRetainedProductArtifact>);
static_assert(!std::is_aggregate_v<
              deployment::VerifiedRetainedProductArtifact>);
static_assert(!std::is_default_constructible_v<
              deployment::VerifiedRetainedModelMap>);
static_assert(!std::is_aggregate_v<
              deployment::VerifiedRetainedModelMap>);
static_assert(!std::is_default_constructible_v<
              deployment::VerifiedRetainedProjectionInput>);
static_assert(!std::is_aggregate_v<
              deployment::VerifiedRetainedProjectionInput>);

[[nodiscard]] core::LogicalSceneSnapshotRoot
make_projection_runtime_shadow_snapshot(const core::CellId& cell_id) {
  core::CollisionPolicySnapshot policy;
  policy.revision = {
      1U, std::string(core::kCanonicalEmptyCollisionPolicyDigest)};
  auto snapshot = std::make_shared<core::LogicalSceneSnapshotDTO>(
      core::LogicalSceneSnapshotDTO{
          core::SceneRevision{
              core::logical_scene_identity::MintSceneId({
                  cell_id,
                  core::BootstrapId("i3_projection_runtime_shadow"),
                  core::BootstrapGeneration(1U),
              }),
              1U,
              core::ContentDigest(std::string(kZeroDigest))},
          cell_id,
          0U,
          0U,
          0U,
          std::move(policy),
          {},
          {},
      });
  snapshot->revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(
          *snapshot);
  if (!snapshot->locally_valid()) {
    throw std::logic_error(
        "test-only projection runtime-shadow snapshot is invalid");
  }
  return core::LogicalSceneSnapshotRoot(std::move(snapshot));
}

class ScopedTemporaryRoot final {
 public:
  ScopedTemporaryRoot() {
    std::filesystem::path pattern =
        std::filesystem::temp_directory_path() /
        "manyforge-lite6-retained-consumer-XXXXXX";
    std::string owned_pattern = pattern.string();
    char* created = ::mkdtemp(owned_pattern.data());
    if (created == nullptr) {
      throw std::runtime_error(
          "cannot create retained Lite6 consumer test root");
    }
    path_ = created;
  }

  ~ScopedTemporaryRoot() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  ScopedTemporaryRoot(const ScopedTemporaryRoot&) = delete;
  ScopedTemporaryRoot& operator=(const ScopedTemporaryRoot&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

class ScopedCurrentDirectory final {
 public:
  explicit ScopedCurrentDirectory(
      const std::filesystem::path& replacement)
      : previous_(std::filesystem::current_path()) {
    std::filesystem::current_path(replacement);
  }

  ~ScopedCurrentDirectory() {
    std::error_code ignored;
    std::filesystem::current_path(previous_, ignored);
  }

  ScopedCurrentDirectory(const ScopedCurrentDirectory&) = delete;
  ScopedCurrentDirectory& operator=(const ScopedCurrentDirectory&) = delete;

 private:
  std::filesystem::path previous_;
};

[[nodiscard]] deployment::VerifiedRetainedPlanningModel
load_lite6_planning_model(
    const std::filesystem::path& package_root,
    const deployment::RetainedPlanningModelQuery& query) {
  const deployment::FilesystemVerifiedRetainedPlanningModelSource source(
      package_root);
  auto model = source.get_verified(query);
  if (!model.has_value()) {
    throw std::logic_error(
        "durable Lite6 planning model was absent on exact point read");
  }
  return std::move(*model);
}

[[nodiscard]] deployment::VerifiedRetainedModelMap load_lite6_model_map(
    const std::filesystem::path& package_root,
    const deployment::RetainedModelMapQuery& query) {
  const deployment::FilesystemVerifiedRetainedModelMapSource source(
      package_root);
  auto model_map = source.get_verified(query);
  if (!model_map.has_value()) {
    throw std::logic_error(
        "durable Lite6 model map was absent on exact point read");
  }
  return std::move(*model_map);
}

void expect_exact_planning_reopen(
    const deployment::VerifiedRetainedPlanningModel& first,
    const deployment::VerifiedRetainedPlanningModel& second) {
  EXPECT_EQ(first.query(), second.query());
  EXPECT_EQ(first.urdf_bytes(), second.urdf_bytes());
  EXPECT_EQ(first.srdf_bytes(), second.srdf_bytes());
  ASSERT_EQ(first.resources().size(), second.resources().size());
  for (std::size_t index = 0U; index < first.resources().size(); ++index) {
    const auto& left = first.resources()[index];
    const auto& right = second.resources()[index];
    EXPECT_EQ(left.logical_uri(), right.logical_uri());
    EXPECT_EQ(left.media_type(), right.media_type());
    EXPECT_EQ(left.byte_size(), right.byte_size());
    EXPECT_EQ(left.content_digest(), right.content_digest());
    EXPECT_EQ(left.content_addressed_store_key(),
              right.content_addressed_store_key());
    EXPECT_EQ(left.bytes(), right.bytes());
  }
}

struct Lite6SceneFacts final {
  std::size_t joint_count;
  std::size_t configuration_size;
  std::size_t velocity_size;
  std::size_t frame_count;
  std::size_t geometry_count;
  std::size_t collision_pair_count;
  std::vector<std::string> independent_joint_names;
  std::vector<std::string> mimic_expanded_joint_names;

  friend bool operator==(const Lite6SceneFacts&,
                         const Lite6SceneFacts&) = default;
};

[[nodiscard]] Lite6SceneFacts require_meaningful_lite6_scene(
    const roboplan::Scene& scene) {
  const auto& model = scene.getModel();
  const auto independent = scene.getJointNames();
  const auto with_mimics = scene.getJointNamesWithMimics();
  const auto& collision_model = scene.getCollisionModel();

  EXPECT_GT(model.njoints, 1);
  EXPECT_GE(model.nq, 7);
  EXPECT_GE(model.nv, 7);
  EXPECT_GE(model.nframes, 10);
  EXPECT_GE(independent.size(), 7U);
  EXPECT_GT(with_mimics.size(), independent.size());
  EXPECT_GE(collision_model.geometryObjects.size(), 10U);
  EXPECT_FALSE(collision_model.collisionPairs.empty());
  EXPECT_EQ(scene.getCurrentJointPositions().size(), model.nq);
  EXPECT_NO_THROW(
      scene.computeCollisionDistances(scene.getCurrentJointPositions()));

  return {
      static_cast<std::size_t>(model.njoints),
      static_cast<std::size_t>(model.nq),
      static_cast<std::size_t>(model.nv),
      static_cast<std::size_t>(model.nframes),
      collision_model.geometryObjects.size(),
      collision_model.collisionPairs.size(),
      independent,
      with_mimics,
  };
}

[[nodiscard]] std::filesystem::path staged_resource_path(
    const RetainedPlanningModelStage& stage,
    std::string_view logical_uri) {
  constexpr std::string_view kAssetPrefix = "asset://";
  if (!logical_uri.starts_with(kAssetPrefix)) {
    throw std::logic_error("verified planning resource is not an asset URI");
  }
  return stage.staging_root() /
         std::string(logical_uri.substr(kAssetPrefix.size()));
}

[[nodiscard]] std::string compiled_symbol(
    const deployment::VerifiedRetainedModelMap& model_map,
    deployment::OccurrenceSymbolKind kind,
    std::string_view source_name, std::string_view scope_name = {}) {
  const auto found = std::find_if(
      model_map.symbols().begin(), model_map.symbols().end(),
      [&](const deployment::OccurrenceSymbolRewrite& rewrite) {
        return rewrite.source.model_instance_id.value() ==
                   "lite6_fixture_instance" &&
               rewrite.source.kind == kind &&
               rewrite.source.source_name == source_name &&
               rewrite.source.scope_name == scope_name;
      });
  if (found == model_map.symbols().end()) {
    throw std::logic_error(
        "retained Lite6 model map is missing source symbol " +
        std::string(source_name));
  }
  return found->compiled_name;
}

void expect_total_retained_model_map_semantics(
    const deployment::VerifiedRetainedModelMap& model_map) {
  using SourceKey =
      std::tuple<std::string, deployment::OccurrenceSymbolKind,
                 std::string, std::string>;
  std::set<SourceKey> source_keys;
  std::set<std::string, std::less<>> compiled_names;
  for (const auto& rewrite : model_map.symbols()) {
    EXPECT_TRUE(source_keys
                    .emplace(
                        rewrite.source.model_instance_id.value(),
                        rewrite.source.kind, rewrite.source.scope_name,
                        rewrite.source.source_name)
                    .second);
    EXPECT_TRUE(compiled_names.emplace(rewrite.compiled_name).second);
  }
  ASSERT_EQ(source_keys.size(), model_map.symbols().size());
  ASSERT_EQ(compiled_names.size(), model_map.symbols().size());

  for (const auto& reference : model_map.references()) {
    const auto target = std::find_if(
        model_map.symbols().begin(), model_map.symbols().end(),
        [&](const deployment::OccurrenceSymbolRewrite& candidate) {
          return candidate.source == reference.source_target;
        });
    ASSERT_NE(target, model_map.symbols().end());
    EXPECT_EQ(target->compiled_name, reference.compiled_target_name);
  }

  std::set<std::pair<std::string, std::size_t>> coordinate_keys;
  for (const auto& coordinate : model_map.coordinates()) {
    EXPECT_TRUE(coordinate_keys
                    .emplace(coordinate.model_instance_id.value(),
                             coordinate.source_joint_index)
                    .second);
    EXPECT_EQ(
        coordinate.compiled_joint_name,
        compiled_symbol(model_map,
                        deployment::OccurrenceSymbolKind::kJoint,
                        coordinate.source_joint_name));
  }
  EXPECT_EQ(coordinate_keys.size(), model_map.coordinates().size());
}

void expect_exact_retained_planning_resources(
    const deployment::VerifiedRetainedPlanningModel& retained) {
  std::set<std::string, std::less<>> retained_uris;
  for (const auto& resource : retained.resources()) {
    EXPECT_TRUE(
        retained_uris.emplace(std::string(resource.logical_uri())).second);
    EXPECT_EQ(resource.byte_size(), resource.bytes().size());
    EXPECT_EQ(resource.content_digest().value(),
              deployment::sha256_digest(resource.bytes()));
    EXPECT_EQ(
        resource.content_addressed_store_key().value(),
        "sha256/" + resource.content_digest().value().substr(
                        std::string_view("sha256:").size()));
  }
}

[[nodiscard]] double mimic_expanded_position(
    const roboplan::Scene& scene, std::string_view joint_name) {
  const auto names = scene.getJointNamesWithMimics();
  const auto values = scene.getCurrentJointPositionsWithMimics();
  const auto found =
      std::find(names.begin(), names.end(), joint_name);
  if (found == names.end()) {
    throw std::logic_error(
        "RoboPlan mimic-expanded state is missing " +
        std::string(joint_name));
  }
  return values[static_cast<Eigen::Index>(
      std::distance(names.begin(), found))];
}

void expect_exact_retained_and_roboplan_mimic_semantics(
    const deployment::VerifiedRetainedModelMap& model_map,
    roboplan::Scene& scene) {
  for (std::size_t index = 0U; index < model_map.coordinates().size();
       ++index) {
    const auto& coordinate = model_map.coordinates()[index];
    EXPECT_EQ(coordinate.model_instance_id.value(),
              "lite6_fixture_instance");
    EXPECT_EQ(coordinate.source_joint_index, index);
    EXPECT_EQ(
        coordinate.compiled_joint_name,
        compiled_symbol(
            model_map, deployment::OccurrenceSymbolKind::kJoint,
            coordinate.source_joint_name));
    if (coordinate.mimic_source_compiled_joint_name.has_value()) {
      const auto master = std::find_if(
          model_map.coordinates().begin(), model_map.coordinates().end(),
          [&](const deployment::CompositionModelCoordinate& candidate) {
            return candidate.model_instance_id ==
                       coordinate.model_instance_id &&
                   candidate.compiled_joint_name ==
                       *coordinate.mimic_source_compiled_joint_name;
          });
      ASSERT_NE(master, model_map.coordinates().end());
      EXPECT_NE(master->source_joint_name, coordinate.source_joint_name);
    }
  }

  const std::string master = compiled_symbol(
      model_map, deployment::OccurrenceSymbolKind::kJoint,
      "right_finger_joint");
  const std::string follower = compiled_symbol(
      model_map, deployment::OccurrenceSymbolKind::kJoint,
      "left_finger_joint");
  const std::string gripper_group = compiled_symbol(
      model_map, deployment::OccurrenceSymbolKind::kSrdfGroup,
      "lite_gripper");
  EXPECT_LT(master, follower);
  const auto follower_coordinate = std::find_if(
      model_map.coordinates().begin(), model_map.coordinates().end(),
      [&follower](const deployment::CompositionModelCoordinate& candidate) {
        return candidate.compiled_joint_name == follower;
      });
  ASSERT_NE(follower_coordinate, model_map.coordinates().end());
  ASSERT_TRUE(
      follower_coordinate->mimic_source_compiled_joint_name.has_value());
  EXPECT_EQ(*follower_coordinate->mimic_source_compiled_joint_name, master);
  EXPECT_DOUBLE_EQ(follower_coordinate->mimic_multiplier, 1.0);
  EXPECT_DOUBLE_EQ(follower_coordinate->mimic_offset, 0.0);

  const auto group = scene.getJointGroupInfo(gripper_group);
  ASSERT_TRUE(group.has_value()) << group.error();
  EXPECT_EQ(
      group->joint_names,
      (std::vector<std::string>{follower, master}));
  EXPECT_EQ(
      static_cast<std::size_t>(group->q_indices.size()), 1U);
  const auto independent = scene.getJointNames();
  const auto expanded = scene.getJointNamesWithMimics();
  std::vector<std::string> expected_independent;
  for (int index = 1; index <= 6; ++index) {
    expected_independent.push_back(compiled_symbol(
        model_map, deployment::OccurrenceSymbolKind::kJoint,
        "joint" + std::to_string(index)));
  }
  expected_independent.push_back(master);
  auto expected_expanded = expected_independent;
  expected_expanded.push_back(follower);
  EXPECT_EQ(scene.getModel().nq, 7);
  EXPECT_EQ(scene.getModel().nv, 7);
  EXPECT_EQ(independent.size(), 7U);
  EXPECT_EQ(expanded.size(), 8U);
  EXPECT_EQ(independent, expected_independent);
  EXPECT_EQ(expanded, expected_expanded);
  EXPECT_EQ(
      std::count(independent.begin(), independent.end(), master), 1);
  EXPECT_EQ(
      std::count(independent.begin(), independent.end(), follower), 0);
  EXPECT_EQ(std::count(expanded.begin(), expanded.end(), master), 1);
  EXPECT_EQ(std::count(expanded.begin(), expanded.end(), follower), 1);

  auto positions = scene.getCurrentJointPositions();
  const auto master_id = scene.getModel().getJointId(master);
  ASSERT_LT(master_id, scene.getModel().idx_qs.size());
  positions[scene.getModel().idx_qs[master_id]] = 0.004;
  scene.setJointPositions(positions);
  EXPECT_NEAR(mimic_expanded_position(scene, master), 0.004, 1e-12);
  EXPECT_NEAR(mimic_expanded_position(scene, follower), 0.004, 1e-12);
}

[[nodiscard]] deployment::UrdfJointIr synthetic_joint(
    std::string name, std::string parent_link, std::string child_link,
    deployment::UrdfJointType type,
    std::optional<std::string> mimic_master = std::nullopt) {
  deployment::UrdfJointIr joint{};
  joint.name = std::move(name);
  joint.parent_link_name = std::move(parent_link);
  joint.child_link_name = std::move(child_link);
  joint.type = type;
  joint.coordinate_kind =
      type == deployment::UrdfJointType::kFixed
          ? deployment::UrdfCoordinateKind::kNone
          : deployment::UrdfCoordinateKind::kLinear;
  if (mimic_master.has_value()) {
    joint.mimic = deployment::UrdfMimicIr{
        .joint_name = std::move(*mimic_master),
        .authored_multiplier = 1.0,
        .authored_offset = 0.0,
        .multiplier = 1.0,
        .offset = 0.0,
    };
  }
  return joint;
}

[[nodiscard]] std::string synthetic_mf2_name(
    std::size_t rank, std::string_view source_name) {
  return deployment::detail::make_mf2_generated_joint_name(
      rank,
      {
          {"fixture", "pinocchio_cross_branch"},
          {"source", source_name},
      });
}

[[nodiscard]] deployment::RetainedProductArtifactQuery artifact_query(
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission,
    std::string_view role) {
  const auto& product =
      admission.retained_bundle()
          .value()
          .compiled_products.planning_collision_model;
  return {
      admission.retained_bundle_id(), product.product_id,
      product.product_kind, core::ArtifactRoleId(std::string(role))};
}

[[nodiscard]] deployment::VerifiedRetainedProductArtifact
require_artifact(
    const deployment::IVerifiedRetainedProductArtifactSource& source,
    const deployment::RetainedProductArtifactQuery& query) {
  std::optional<deployment::VerifiedRetainedProductArtifact> artifact =
      source.get_verified(query);
  if (!artifact.has_value()) {
    throw std::runtime_error(
        "canonical-empty retained product artifact is absent");
  }
  return std::move(*artifact);
}

void expect_canonical_empty_scene(const roboplan::Scene& scene) {
  ASSERT_EQ(scene.getModel().njoints, 1);
  ASSERT_EQ(scene.getModel().names.size(), 1U);
  EXPECT_EQ(scene.getModel().names.front(), "universe");
  ASSERT_EQ(scene.getModel().nframes, 2);
  const auto cell_frame_id =
      scene.getModel().getFrameId(std::string(kCanonicalCellFrame));
  ASSERT_EQ(cell_frame_id, 1U);
  const auto& cell_frame = scene.getModel().frames[cell_frame_id];
  EXPECT_EQ(cell_frame.parentJoint, 0U);
  EXPECT_TRUE(
      cell_frame.placement.toHomogeneousMatrix().isApprox(
          Eigen::Matrix4d::Identity()));
  EXPECT_EQ(scene.getModel().nq, 0);
  EXPECT_EQ(scene.getModel().nv, 0);
  EXPECT_TRUE(scene.getJointNames().empty());
  EXPECT_TRUE(scene.getJointNamesWithMimics().empty());
  EXPECT_EQ(scene.getCurrentJointPositions().size(), 0);
  EXPECT_TRUE(scene.getCollisionModel().geometryObjects.empty());
  EXPECT_TRUE(scene.getCollisionModel().collisionPairs.empty());

  const Eigen::VectorXd empty_configuration(0);
  EXPECT_FALSE(scene.hasCollisions(empty_configuration));
  EXPECT_NO_THROW(scene.computeCollisionDistances(empty_configuration));
  EXPECT_TRUE(scene.getCollisionData().distanceResults.empty());
}

TEST(RetainedProductRoboPlanConsumer,
     Mf2CrossBranchTraversalImportsDeeperMasterBeforeFollower) {
  deployment::UrdfDocumentIr source;
  source.robot_name = "mf2_cross_branch";
  source.joints = {
      synthetic_joint(
          "a_follower_branch", "root", "follower_branch",
          deployment::UrdfJointType::kFixed),
      synthetic_joint(
          "follower", "follower_branch", "follower_tip",
          deployment::UrdfJointType::kPrismatic, "master"),
      synthetic_joint(
          "z_master_branch", "root", "master_branch",
          deployment::UrdfJointType::kFixed),
      synthetic_joint(
          "master", "master_branch", "master_tip",
          deployment::UrdfJointType::kPrismatic),
  };
  const auto rank_rows =
      deployment::detail::dependency_ordered_joint_local_ranks(source);
  const std::map<std::string, std::size_t, std::less<>> ranks(
      rank_rows.begin(), rank_rows.end());
  ASSERT_EQ(ranks.at("z_master_branch"), 0U);
  ASSERT_EQ(ranks.at("a_follower_branch"), 1U);
  const std::string master_branch = synthetic_mf2_name(
      ranks.at("z_master_branch"), "z_master_branch");
  const std::string follower_branch = synthetic_mf2_name(
      ranks.at("a_follower_branch"), "a_follower_branch");
  const std::string master =
      synthetic_mf2_name(ranks.at("master"), "master");
  const std::string follower =
      synthetic_mf2_name(ranks.at("follower"), "follower");
  ASSERT_LT(master_branch, follower_branch);

  const std::string urdf =
      "<robot name=\"mf2_cross_branch\">"
      "<link name=\"root\"/>"
      "<link name=\"follower_branch\"/>"
      "<joint name=\"" +
      follower_branch +
      "\" type=\"fixed\"><parent link=\"root\"/><child "
      "link=\"follower_branch\"/></joint>"
      "<link name=\"follower_tip\"/>"
      "<joint name=\"" +
      follower +
      "\" type=\"prismatic\"><parent link=\"follower_branch\"/><child "
      "link=\"follower_tip\"/><axis xyz=\"1 0 0\"/><limit lower=\"-1\" "
      "upper=\"1\" effort=\"1\" velocity=\"1\"/><mimic joint=\"" +
      master +
      "\" multiplier=\"1\" offset=\"0\"/></joint>"
      "<link name=\"master_branch\"/>"
      "<joint name=\"" +
      master_branch +
      "\" type=\"fixed\"><parent link=\"root\"/><child "
      "link=\"master_branch\"/></joint>"
      "<link name=\"master_tip\"/>"
      "<joint name=\"" +
      master +
      "\" type=\"prismatic\"><parent link=\"master_branch\"/><child "
      "link=\"master_tip\"/><axis xyz=\"1 0 0\"/><limit lower=\"-1\" "
      "upper=\"1\" effort=\"1\" velocity=\"1\"/></joint>"
      "</robot>";
  const std::string srdf =
      "<robot name=\"mf2_cross_branch\"><group name=\"motion\">"
      "<joint name=\"" +
      follower + "\"/><joint name=\"" + master +
      "\"/></group></robot>";

  roboplan::Scene scene("mf2_cross_branch", urdf, srdf);
  EXPECT_EQ(scene.getModel().nq, 1);
  EXPECT_EQ(scene.getModel().nv, 1);
  EXPECT_EQ(
      scene.getJointNames(), (std::vector<std::string>{master}));
  EXPECT_EQ(
      scene.getJointNamesWithMimics(),
      (std::vector<std::string>{master, follower}));
  const auto group = scene.getJointGroupInfo("motion");
  ASSERT_TRUE(group.has_value()) << group.error();
  EXPECT_EQ(
      group->joint_names,
      (std::vector<std::string>{follower, master}));
  EXPECT_EQ(static_cast<std::size_t>(group->q_indices.size()), 1U);
  auto positions = scene.getCurrentJointPositions();
  ASSERT_EQ(positions.size(), 1);
  positions[0] = 0.25;
  scene.setJointPositions(positions);
  EXPECT_NEAR(mimic_expanded_position(scene, master), 0.25, 1e-12);
  EXPECT_NEAR(mimic_expanded_position(scene, follower), 0.25, 1e-12);
}

TEST(RetainedProductRoboPlanConsumer,
     CanonicalEmptyExactBytesYieldDeterministicPlanningCollisionScene) {
  const std::filesystem::path package_root(
      MANYFORGE_CANONICAL_EMPTY_PRODUCT_PACKAGE_ROOT);
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource
      admission_source(package_root);
  const auto admission = admission_source.load_verified();
  const deployment::FilesystemVerifiedRetainedProductArtifactSource
      product_source(package_root);

  const auto urdf_query = artifact_query(admission, "urdf");
  const auto srdf_query = artifact_query(admission, "srdf");
  const auto urdf = require_artifact(product_source, urdf_query);
  const auto srdf = require_artifact(product_source, srdf_query);
  EXPECT_EQ(urdf.query(), urdf_query);
  EXPECT_EQ(srdf.query(), srdf_query);
  EXPECT_EQ(urdf.product_contract_id(), srdf.product_contract_id());
  EXPECT_EQ(urdf.product_contract_revision(),
            srdf.product_contract_revision());
  EXPECT_EQ(urdf.artifact_ref().media_type, "application/xml");
  EXPECT_EQ(srdf.artifact_ref().media_type, "application/xml");

  const roboplan::Scene first_scene(
      admission.cell_id().value(), std::string(urdf.bytes()),
      std::string(srdf.bytes()),
      std::vector<std::filesystem::path>{});
  expect_canonical_empty_scene(first_scene);

  const deployment::FilesystemCanonicalEmptyRetainedBundleSource
      reopened_admission_source(package_root);
  const auto reopened_admission =
      reopened_admission_source.load_verified();
  const deployment::FilesystemVerifiedRetainedProductArtifactSource
      reopened_product_source(package_root);
  const auto reopened_urdf = require_artifact(
      reopened_product_source,
      artifact_query(reopened_admission, "urdf"));
  const auto reopened_srdf = require_artifact(
      reopened_product_source,
      artifact_query(reopened_admission, "srdf"));

  EXPECT_EQ(reopened_admission.retained_bundle_id(),
            admission.retained_bundle_id());
  EXPECT_EQ(reopened_urdf.query(), urdf.query());
  EXPECT_EQ(reopened_srdf.query(), srdf.query());
  EXPECT_EQ(reopened_urdf.product_contract_id(),
            urdf.product_contract_id());
  EXPECT_EQ(reopened_srdf.product_contract_id(),
            srdf.product_contract_id());
  EXPECT_EQ(reopened_urdf.product_contract_revision(),
            urdf.product_contract_revision());
  EXPECT_EQ(reopened_srdf.product_contract_revision(),
            srdf.product_contract_revision());
  EXPECT_EQ(reopened_urdf.artifact_ref(), urdf.artifact_ref());
  EXPECT_EQ(reopened_srdf.artifact_ref(), srdf.artifact_ref());
  EXPECT_EQ(reopened_urdf.bytes(), urdf.bytes());
  EXPECT_EQ(reopened_srdf.bytes(), srdf.bytes());

  const roboplan::Scene reopened_scene(
      reopened_admission.cell_id().value(),
      std::string(reopened_urdf.bytes()),
      std::string(reopened_srdf.bytes()),
      std::vector<std::filesystem::path>{});
  expect_canonical_empty_scene(reopened_scene);
  EXPECT_EQ(reopened_scene.getModel().nframes,
            first_scene.getModel().nframes);
  EXPECT_EQ(reopened_scene.getCollisionModel().geometryObjects.size(),
            first_scene.getCollisionModel().geometryObjects.size());
}

TEST(RetainedProductRoboPlanConsumer,
     DurableLite6ReopensStagesAndBuildsDeterministicNonEmptyScene) {
  ScopedTemporaryRoot temporary;
  const std::filesystem::path package_root =
      temporary.path() / "retained-package";
  const std::filesystem::path staging_parent =
      temporary.path() / "private-stages";
  const std::filesystem::path empty_ambient_root =
      temporary.path() / "empty-ambient";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));
  ASSERT_TRUE(std::filesystem::create_directory(staging_parent));
  ASSERT_TRUE(std::filesystem::create_directory(empty_ambient_root));

  const auto materialized =
      deployment::test::materialize_lite6_retained_consumer_fixture(
          package_root);
  ASSERT_NE(materialized.bundle, nullptr);
  ASSERT_EQ(
      materialized.bundle->value().provenance.generator_records.size(),
      1U);
  EXPECT_EQ(
      materialized.bundle->value().provenance.generator_records.front(),
      materialized.input.preparation.generator_record());
  const auto& planning =
      materialized.bundle->value()
          .compiled_products.planning_collision_model;
  const deployment::RetainedPlanningModelQuery query{
      materialized.bundle->retained_bundle_id(), planning.product_id};
  ASSERT_EQ(
      materialized.bundle->value().compiled_products.symbol_role_maps.size(),
      1U);
  const auto& symbol_product =
      materialized.bundle->value().compiled_products.symbol_role_maps.front();
  const deployment::RetainedModelMapQuery model_map_query{
      materialized.bundle->retained_bundle_id(), symbol_product.product_id};

  const deployment::FilesystemVerifiedRetainedPlanningModelSource
      projection_source(package_root);
  const auto first_projection =
      deployment::verify_retained_projection_input(
          *materialized.bundle,
          make_projection_runtime_shadow_snapshot(
              materialized.bundle->value().cell_graph.cell_id),
          projection_source);
  const auto& first = first_projection.planning_model();
  EXPECT_EQ(
      first_projection.projection_revision(),
      (core::ProjectionRevisionDTO{
          materialized.bundle->retained_bundle_id(),
          materialized.bundle->value().cell_graph.graph_revision,
          first_projection.logical_scene().shared()->revision}));
  const auto model_map =
      load_lite6_model_map(package_root, model_map_query);
  expect_total_retained_model_map_semantics(model_map);
  ASSERT_NO_THROW(model_map.require_same_bundle_as(first));
  ASSERT_EQ(model_map.query().retained_bundle_id,
            first.query().retained_bundle_id);
  const core::ControllableComponentId component_id(
      std::string(deployment::test::kLite6ControllableComponentId));
  const core::CommandTargetId command_target_id(
      std::string(deployment::test::kLite6ArmTargetId));
  const auto* arm_binding =
      model_map.find_binding(component_id, command_target_id);
  const auto* arm_target =
      model_map.find_target(component_id, command_target_id);
  ASSERT_NE(arm_binding, nullptr);
  ASSERT_NE(arm_target, nullptr);
  ASSERT_FALSE(arm_target->model_groups.empty());
  ASSERT_EQ(arm_binding->resources.size(), 6U);
  ASSERT_FALSE(first.resources().empty());
  expect_exact_retained_planning_resources(first);
  const std::filesystem::path generated_fixture_root(
      MANYFORGE_LITE6_RETAINED_CONSUMER_FIXTURE_ROOT);
  EXPECT_EQ(
      first.urdf_bytes(),
      deployment::test::read_lite6_fixture_file(
          generated_fixture_root / "retained-model.urdf"));
  EXPECT_EQ(
      first.srdf_bytes(),
      deployment::test::read_lite6_fixture_file(
          generated_fixture_root / "retained-model.srdf"));

  // This unrelated valid CAS blob must never become an implicit model
  // resource merely because it is present under the package root.
  const deployment::StoredRawArtifact ambient_blob = [&]() {
    deployment::FilesystemRawArtifactStore raw_store(package_root);
    return raw_store.put_if_absent(
        "unreferenced ambient retained bytes");
  }();
  for (const auto& resource : first.resources()) {
    EXPECT_NE(resource.content_digest(), ambient_blob.content_digest);
    EXPECT_NE(resource.bytes(), "unreferenced ambient retained bytes");
  }

  const auto reopened_projection =
      deployment::verify_retained_projection_input(
          *materialized.bundle,
          make_projection_runtime_shadow_snapshot(
              materialized.bundle->value().cell_graph.cell_id),
          projection_source);
  const auto& reopened = reopened_projection.planning_model();
  EXPECT_EQ(reopened_projection.projection_revision(),
            first_projection.projection_revision());
  EXPECT_EQ(reopened_projection.logical_scene().shared()->identity(),
            first_projection.logical_scene().shared()->identity());
  EXPECT_TRUE(core::IsCanonicalEmptyCollisionPolicy(
      reopened_projection.logical_scene()
          .shared()
          ->configured_collision_policy));
  EXPECT_TRUE(reopened_projection.logical_scene().shared()->objects.empty());
  EXPECT_TRUE(reopened_projection.logical_scene()
                  .shared()
                  ->carried_object_attachments.empty());
  expect_exact_planning_reopen(first, reopened);

  std::filesystem::path first_stage_root;
  std::string first_loader_urdf;
  Lite6SceneFacts first_scene_facts{};
  {
    auto stage = RetainedPlanningModelStage::create_verified(
        first, staging_parent);
    first_stage_root = stage.staging_root();
    first_loader_urdf = stage.loader_urdf_bytes();
    ASSERT_EQ(stage.package_paths().size(), 1U);
    EXPECT_EQ(stage.package_paths().front(), stage.staging_root());
    EXPECT_EQ(stage.staging_root().parent_path(), staging_parent);
    EXPECT_EQ(stage.loader_urdf_bytes().find("asset://"),
              std::string_view::npos);
    EXPECT_EQ(stage.loader_urdf_bytes().find("file://"),
              std::string_view::npos);
    EXPECT_EQ(stage.loader_urdf_bytes().find("model://"),
              std::string_view::npos);
    EXPECT_EQ(
        stage.loader_urdf_bytes().find(
            "package://manyforge_example_models/"),
        std::string_view::npos);
    for (const auto& resource : first.resources()) {
      EXPECT_TRUE(std::filesystem::is_regular_file(
          staged_resource_path(stage, resource.logical_uri())));
    }

    ScopedCurrentDirectory no_ambient_lookup(empty_ambient_root);
    try {
      roboplan::Scene scene(
          "retained_lite6", std::string(stage.loader_urdf_bytes()),
          std::string(stage.srdf_bytes()), stage.package_paths());
      first_scene_facts = require_meaningful_lite6_scene(scene);
      expect_exact_retained_and_roboplan_mimic_semantics(
          model_map, scene);

      const std::string master = compiled_symbol(
          model_map, deployment::OccurrenceSymbolKind::kJoint,
          "right_finger_joint");
      auto positions = scene.getCurrentJointPositions();
      const auto master_id = scene.getModel().getJointId(master);
      ASSERT_LT(master_id, scene.getModel().idx_qs.size());
      positions[scene.getModel().idx_qs[master_id]] = 0.004;
      scene.setJointPositions(positions);
      EXPECT_NEAR(mimic_expanded_position(scene, master), 0.004, 1e-12);
    } catch (const std::exception& error) {
      ADD_FAILURE() << error.what() << "\n" << first.urdf_bytes();
      return;
    }
  }
  EXPECT_FALSE(std::filesystem::exists(first_stage_root));

  std::filesystem::path reopened_stage_root;
  Lite6SceneFacts reopened_scene_facts{};
  {
    auto stage = RetainedPlanningModelStage::create_verified(
        reopened, staging_parent);
    reopened_stage_root = stage.staging_root();
    EXPECT_NE(reopened_stage_root, first_stage_root);
    EXPECT_EQ(stage.loader_urdf_bytes(), first_loader_urdf);

    ScopedCurrentDirectory no_ambient_lookup(empty_ambient_root);
    const roboplan::Scene scene(
        "retained_lite6", std::string(stage.loader_urdf_bytes()),
        std::string(stage.srdf_bytes()), stage.package_paths());
    reopened_scene_facts = require_meaningful_lite6_scene(scene);
  }
  EXPECT_EQ(reopened_scene_facts, first_scene_facts);
  EXPECT_FALSE(std::filesystem::exists(reopened_stage_root));

  // Removing a selected staged file must make a fresh real Scene load fail.
  // If Pinocchio/RoboPlan searched an ambient ROS/package location, this
  // could incorrectly keep succeeding.
  std::filesystem::path incomplete_stage_root;
  {
    auto stage = RetainedPlanningModelStage::create_verified(
        reopened, staging_parent);
    incomplete_stage_root = stage.staging_root();
    const auto selected = std::find_if(
        reopened.resources().begin(), reopened.resources().end(),
        [&stage](const deployment::VerifiedRetainedPlanningResource&
                     resource) {
          const std::string package_uri =
              "package://" +
              std::string(resource.logical_uri().substr(
                  std::string_view("asset://").size()));
          return stage.loader_urdf_bytes().find(package_uri) !=
                 std::string_view::npos;
        });
    ASSERT_NE(selected, reopened.resources().end());
    const std::filesystem::path selected_path =
        staged_resource_path(stage, selected->logical_uri());
    const std::filesystem::path hidden_path =
        selected_path.string() + ".missing";
    ASSERT_NO_THROW(std::filesystem::rename(selected_path, hidden_path));

    ScopedCurrentDirectory no_ambient_lookup(empty_ambient_root);
    EXPECT_ANY_THROW(
        static_cast<void>(roboplan::Scene(
            "retained_lite6_missing_resource",
            std::string(stage.loader_urdf_bytes()),
            std::string(stage.srdf_bytes()), stage.package_paths())));
  }
  EXPECT_FALSE(std::filesystem::exists(incomplete_stage_root));
  EXPECT_TRUE(std::filesystem::is_empty(staging_parent));
}

TEST(RetainedProductCrossConsumer, DurableLite6ModelPreservesAuthoritativeCollisionDecisions) {
  ScopedTemporaryRoot temporary;
  const std::filesystem::path package_root = temporary.path() / "retained-package";
  const std::filesystem::path staging_parent = temporary.path() / "private-stages";
  const std::filesystem::path empty_ambient_root = temporary.path() / "empty-ambient";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));
  ASSERT_TRUE(std::filesystem::create_directory(staging_parent));
  ASSERT_TRUE(std::filesystem::create_directory(empty_ambient_root));

  const auto materialized =
      deployment::test::materialize_lite6_retained_consumer_fixture(package_root);
  ASSERT_NE(materialized.bundle, nullptr);
  const auto& products = materialized.bundle->value().compiled_products;
  const deployment::RetainedPlanningModelQuery planning_query{
      materialized.bundle->retained_bundle_id(), products.planning_collision_model.product_id};
  ASSERT_EQ(products.symbol_role_maps.size(), 1U);
  const deployment::RetainedModelMapQuery map_query{materialized.bundle->retained_bundle_id(),
                                                    products.symbol_role_maps.front().product_id};
  const auto retained = load_lite6_planning_model(package_root, planning_query);
  const auto model_map = load_lite6_model_map(package_root, map_query);
  model_map.require_same_bundle_as(retained);

  const core::ControllableComponentId component_id(
      std::string(deployment::test::kLite6ControllableComponentId));
  const core::CommandTargetId command_target_id(std::string(deployment::test::kLite6ArmTargetId));
  const auto* arm_binding = model_map.find_binding(component_id, command_target_id);
  const auto* arm_target = model_map.find_target(component_id, command_target_id);
  ASSERT_NE(arm_binding, nullptr);
  ASSERT_NE(arm_target, nullptr);
  const auto arm_group =
      std::find_if(arm_target->model_groups.begin(), arm_target->model_groups.end(),
                   [](const deployment::CompositionTargetGroupResolution& group) {
                     return group.model_group_role_id.value() == "planning_group";
                   });
  ASSERT_NE(arm_group, arm_target->model_groups.end());
  const auto tcp_frame =
      std::find_if(arm_target->frames.begin(), arm_target->frames.end(),
                   [](const deployment::CompositionTargetFrameResolution& frame) {
                     return frame.frame_role_id.value() == "tcp";
                   });
  ASSERT_NE(tcp_frame, arm_target->frames.end());
  const auto* component_tcp =
      std::get_if<deployment::CompositionComponentModelFrame>(&tcp_frame->frame);
  ASSERT_NE(component_tcp, nullptr);
  const std::string motion_link = component_tcp->compiled_frame_name;

  std::vector<std::string> arm_joint_names;
  for (const auto& resource : arm_binding->resources) {
    ASSERT_EQ(resource.coordinates.size(), 1U);
    EXPECT_FALSE(resource.coordinates.front().derived_mimic);
    arm_joint_names.push_back(resource.coordinates.front().compiled_joint_name);
  }
  ASSERT_EQ(arm_joint_names.size(), 6U);

  auto stage = RetainedPlanningModelStage::create_verified(retained, staging_parent);
  ScopedCurrentDirectory no_ambient_lookup(empty_ambient_root);
  roboplan::Scene planning_scene("retained_lite6_cross_consumer",
                                 std::string(stage.loader_urdf_bytes()),
                                 std::string(stage.srdf_bytes()), stage.package_paths());
  ManyForgeCollisionEngine collision_engine("retained_lite6_cross_consumer",
                                            stage.loader_urdf_path(), stage.srdf_path(),
                                            stage.package_paths(), arm_group->compiled_group_name);

  const std::string robot_body_id = "robot/lite6/link/" + motion_link + "/collision/0";
  const std::string fixture_body_id = "fixture/retained-model-obstacle/shape/0";
  const auto make_box_geometry = [](const core::CollisionBodyId& body_id,
                                    const core::CollisionOwnerKind owner_kind,
                                    const double extent_m) {
    core::CollisionGeometryDescriptor geometry;
    geometry.body_id = body_id;
    geometry.owner_kind = owner_kind;
    geometry.geometry_kind = core::CollisionGeometryKind::kBox;
    geometry.local_pose = core::Pose3DTO{{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
    geometry.box_dimensions_m = core::Vector3DTO{extent_m, extent_m, extent_m};
    return geometry;
  };
  const auto make_snapshot = [&](const std::uint64_t revision_id, const double fixture_x_m,
                                 const double fixture_extent_m) {
    core::CollisionWorldBody robot_body;
    robot_body.body_id = robot_body_id;
    robot_body.owner_kind = core::CollisionOwnerKind::kRobotLink;
    robot_body.frame_id = motion_link;
    robot_body.pose = core::Pose3DTO{{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
    robot_body.geometries.push_back(
        make_box_geometry(robot_body.body_id, robot_body.owner_kind, 0.05));

    core::CollisionWorldBody fixture_body;
    fixture_body.body_id = fixture_body_id;
    fixture_body.owner_kind = core::CollisionOwnerKind::kFixture;
    fixture_body.frame_id = "universe";
    fixture_body.pose = core::Pose3DTO{{fixture_x_m, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
    fixture_body.geometries.push_back(
        make_box_geometry(fixture_body.body_id, fixture_body.owner_kind, fixture_extent_m));

    auto built = core::CollisionWorldSnapshotBuilder()
                     .set_publisher_id("retained-lite6-cross-consumer")
                     .set_revision_id(revision_id)
                     .add_body(std::move(robot_body))
                     .add_body(std::move(fixture_body))
                     .build();
    if (!built.ok()) {
      throw std::logic_error("cannot construct retained-model parity snapshot: " +
                             built.error().message);
    }
    return built.value();
  };

  std::set<std::string> retained_collision_frames;
  for (const auto& geometry : planning_scene.getCollisionModel().geometryObjects) {
    ASSERT_LT(geometry.parentFrame, planning_scene.getModel().frames.size());
    retained_collision_frames.insert(planning_scene.getModel().frames[geometry.parentFrame].name);
  }
  const Eigen::Matrix4d identity = Eigen::Matrix4d::Identity();
  const Eigen::Vector4d color(0.2, 0.4, 0.8, 1.0);
  const std::string robot_proxy_name = "retained_model_robot_proxy";
  const std::string fixture_name = "retained_model_world_fixture";
  const auto added_proxy = planning_scene.addBoxGeometry(
      robot_proxy_name, motion_link, roboplan::Box(0.05, 0.05, 0.05), identity, color);
  ASSERT_TRUE(added_proxy.has_value()) << added_proxy.error();
  for (const auto& retained_frame : retained_collision_frames) {
    const auto disabled = planning_scene.setCollisions(retained_frame, robot_proxy_name, false);
    ASSERT_TRUE(disabled.has_value()) << disabled.error();
  }
  const auto add_world_fixture = [&](const double x_m, const double extent_m) {
    Eigen::Matrix4d pose = Eigen::Matrix4d::Identity();
    pose(0, 3) = x_m;
    const auto added = planning_scene.addBoxGeometry(
        fixture_name, "universe", roboplan::Box(extent_m, extent_m, extent_m), pose, color);
    ASSERT_TRUE(added.has_value()) << added.error();
    for (const auto& retained_frame : retained_collision_frames) {
      const auto disabled = planning_scene.setCollisions(retained_frame, fixture_name, false);
      ASSERT_TRUE(disabled.has_value()) << disabled.error();
    }
  };
  add_world_fixture(10.0, 0.2);
  auto snapshot = make_snapshot(1U, 10.0, 0.2);

  const auto compare_configuration =
      [&](std::string query_id, const std::vector<std::string>& names,
          const std::vector<double>& values, const bool expected_collision) {
        ASSERT_EQ(names.size(), values.size());
        Eigen::VectorXd planning_positions = Eigen::VectorXd::Zero(planning_scene.getModel().nq);
        for (std::size_t index = 0U; index < names.size(); ++index) {
          const auto joint_id = planning_scene.getModel().getJointId(names[index]);
          ASSERT_LT(joint_id, planning_scene.getModel().idx_qs.size());
          planning_positions[planning_scene.getModel().idx_qs[joint_id]] = values[index];
        }
        const bool planning_collision = planning_scene.hasCollisions(planning_positions);

        core::CollisionQuery query;
        query.query_id = std::move(query_id);
        query.collision_world_revision = snapshot.revision;
        query.robot_state.joint_names = names;
        query.robot_state.positions_rad = values;
        const auto result = collision_engine.check(snapshot, query, core::CollisionPolicySnapshot{},
                                                   core::CollisionCancellationToken{});
        EXPECT_EQ(result.engine_status, core::CollisionEngineStatus::kOk);
        EXPECT_EQ(result.in_collision, planning_collision);
        EXPECT_EQ(result.decision, planning_collision ? core::CollisionDecisionResult::kFail
                                                      : core::CollisionDecisionResult::kPass);
        EXPECT_EQ(planning_collision, expected_collision);
        if (expected_collision) {
          ASSERT_EQ(result.normalized_pairs.size(), 1U);
          EXPECT_EQ(result.normalized_pairs.front().body_a, fixture_body_id);
          EXPECT_EQ(result.normalized_pairs.front().body_b, robot_body_id);
        } else {
          EXPECT_TRUE(result.normalized_pairs.empty());
        }
      };

  const std::vector<double> home(arm_joint_names.size(), 0.0);
  const std::vector<double> historical_rest{0.0, -0.785, 0.785, 0.0, 1.57, 0.0};
  compare_configuration("lite6-home", arm_joint_names, home, false);
  compare_configuration("lite6-historical-rest", arm_joint_names, historical_rest, false);

  const std::string gripper_master =
      compiled_symbol(model_map, deployment::OccurrenceSymbolKind::kJoint, "right_finger_joint");
  compare_configuration("lite6-open-gripper", {gripper_master}, {0.004}, false);

  const auto removed = planning_scene.removeGeometry(fixture_name);
  ASSERT_TRUE(removed.has_value()) << removed.error();
  add_world_fixture(0.0, 4.0);
  snapshot = make_snapshot(2U, 0.0, 4.0);
  compare_configuration("lite6-positive-collision", arm_joint_names, home, true);
}

}  // namespace
}  // namespace manyforge::roboplan_backend
