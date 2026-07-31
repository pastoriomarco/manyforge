#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <Eigen/Core>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <roboplan/core/scene.hpp>

namespace manyforge::roboplan_backend {
namespace {

constexpr std::string_view kCanonicalCellFrame =
    "mf1_cell_e228ede37638a8d03080644d418a75c681930a4b5b3f6b0bbde2603974dab681";

static_assert(!std::is_default_constructible_v<
              deployment::VerifiedRetainedProductArtifact>);
static_assert(!std::is_aggregate_v<
              deployment::VerifiedRetainedProductArtifact>);

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

}  // namespace
}  // namespace manyforge::roboplan_backend
