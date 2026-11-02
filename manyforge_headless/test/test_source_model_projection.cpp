#include <gtest/gtest.h>

#include <filesystem>
#include <unistd.h>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/retained_bundle.hpp>

#include "activation_deployment_intent_internal.hpp"
#include "prepared_activation_materialization_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

#include "assistant_authoring_fixture.hpp"
#include "manyforge_headless/source_model_projection.hpp"

namespace manyforge::headless {
namespace {

namespace content_identity = manyforge::content_identity;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

TEST(SourceModelProjection, CanonicalEmptyBundleHasAClosedEmptyProjection) {
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource source(
      MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  const auto admission = source.load_verified();
  const ActiveSourceModelProjection projection(
      MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);

  const auto value = projection.Project(admission.retained_bundle());
  const auto bytes = content_identity::canonical_json(value);
  EXPECT_NE(bytes.find("\"schema_version\":\"manyforge.source_model_projection.v3\""),
            std::string::npos);
  EXPECT_NE(bytes.find("\"model_map_contract\":null"), std::string::npos);
  EXPECT_NE(bytes.find("\"model_map_product_id\":null"), std::string::npos);
  EXPECT_NE(bytes.find("\"occurrences\":[]"), std::string::npos);
  EXPECT_NE(bytes.find("\"targets\":[]"), std::string::npos);
  EXPECT_NE(bytes.find("\"components\":[]"), std::string::npos);
  EXPECT_EQ(bytes.find("compiled_"), std::string::npos);
  EXPECT_EQ(bytes.find("resource_id"), std::string::npos);
  EXPECT_EQ(bytes.find("control_binding"), std::string::npos);
}

TEST(SourceModelProjection, EmptyScenarioWithRetainedMapProjectsEmpty) {
  using deployment::CanonicalValue;
  namespace detail = deployment::detail;
  const CanonicalValue intent_value(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.deployment_intent.v2")},
      {"deployment_artifact_id", CanonicalValue("empty_projection")},
      {"name", CanonicalValue("empty_projection")},
      {"attachments", CanonicalValue(CanonicalValue::Array{})},
      {"components", CanonicalValue(CanonicalValue::Array{})},
      {"policies", CanonicalValue(CanonicalValue::Object{})},
  });
  const auto intent = detail::parse_activation_deployment_intent(intent_value, {});
  const auto materialized = detail::materialize_prepared_activation(
      MANYFORGE_WORKSPACE_ROOT, {}, intent, core::CellId("empty_projection_cell"), {});
  ASSERT_NE(materialized.bundle, nullptr);
  ASSERT_TRUE(materialized.bundle->value().cell_graph.model_instances.empty());
  ASSERT_EQ(materialized.bundle->value().compiled_products.symbol_role_maps.size(), 1U);

  const auto root = std::filesystem::temp_directory_path() /
      ("manyforge-source-model-empty-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  struct Cleanup {
    std::filesystem::path root;
    ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); }
  } cleanup{root};
  const auto missing_root = root / "missing-map";
  std::filesystem::create_directory(missing_root);
  deployment::FilesystemRawArtifactStore missing_artifacts(missing_root);
  deployment::FilesystemRetainedBundleRepository missing_bundles(missing_root);
  ASSERT_TRUE(missing_bundles.put_if_absent(materialized.bundle));
  EXPECT_THROW(static_cast<void>(ActiveSourceModelProjection(missing_root).Project(
                   *materialized.bundle)), SourceModelProjectionError);

  deployment::FilesystemRawArtifactStore artifacts(root);
  for (const auto& artifact : materialized.raw_artifacts) {
    const auto stored = artifacts.put_if_absent(artifact.bytes);
    EXPECT_EQ(stored.content_addressed_store_key, artifact.content_addressed_store_key);
    EXPECT_EQ(stored.content_digest, artifact.content_digest);
  }
  deployment::FilesystemRetainedBundleRepository bundles(root);
  ASSERT_TRUE(bundles.put_if_absent(materialized.bundle));

  const auto bytes = content_identity::canonical_json(
      ActiveSourceModelProjection(root).Project(*materialized.bundle));
  EXPECT_NE(bytes.find("\"model_map_product_id\":\"cell_composed_model_symbol_role_map\""),
            std::string::npos);
  EXPECT_NE(bytes.find("\"occurrences\":[]"), std::string::npos);
  EXPECT_NE(bytes.find("\"targets\":[]"), std::string::npos);
  EXPECT_NE(bytes.find("\"components\":[]"), std::string::npos);
}

TEST(SourceModelProjection, NonemptyGraphWithoutMapIsRejected) {
  const auto original = test::LoadAssistantAuthoringBundle();
  auto value = original->value();
  ASSERT_FALSE(value.cell_graph.model_instances.empty());
  value.compiled_products.symbol_role_maps.clear();
  const auto inconsistent = deployment::detail::RetainedBundleConstructionAuthority::construct(
      original->retained_bundle_id(), std::move(value), original->normalized_root(),
      std::string(original->canonical_bytes()));
  const ActiveSourceModelProjection projection(MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT);
  EXPECT_THROW(static_cast<void>(projection.Project(*inconsistent)), SourceModelProjectionError);

  value = original->value();
  value.compiled_products.symbol_role_maps.push_back(
      value.compiled_products.symbol_role_maps.front());
  const auto duplicate = deployment::detail::RetainedBundleConstructionAuthority::construct(
      original->retained_bundle_id(), std::move(value), original->normalized_root(),
      std::string(original->canonical_bytes()));
  EXPECT_THROW(static_cast<void>(projection.Project(*duplicate)), SourceModelProjectionError);
}

TEST(SourceModelProjection, RetainedBundleProjectsSourceFactsAndExactViewerBindings) {
  const auto bundle = test::LoadAssistantAuthoringBundle();
  const ActiveSourceModelProjection projection(MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT);

  const auto value = projection.Project(*bundle);
  const auto bytes = content_identity::canonical_json(value);

  EXPECT_NE(bytes.find("\"model_instance_id\":\"lite6_fixture_instance\""), std::string::npos);
  EXPECT_EQ(bytes.find("\"model_instance_id\":\"lite6_fixture_instance_2\""), std::string::npos);
  EXPECT_NE(bytes.find("\"component_id\":\"lite6_arm\""), std::string::npos);
  EXPECT_EQ(bytes.find("\"component_id\":\"lite6_arm_2\""), std::string::npos);
  EXPECT_NE(bytes.find("\"source_joint_name\":\"joint1\""), std::string::npos);
  EXPECT_NE(bytes.find("\"source_parent_link_name\":\"link_base\""), std::string::npos);
  EXPECT_NE(bytes.find("\"source_child_link_name\":\"link1\""), std::string::npos);
  EXPECT_NE(bytes.find("\"mimic\":{\"multiplier\":1,\"offset\":0,"
                       "\"source_joint_name\":\"right_finger_joint\"}"),
            std::string::npos);
  EXPECT_NE(bytes.find("\"links\":["), std::string::npos);
  EXPECT_NE(bytes.find("\"viewer_joint_name\":"), std::string::npos);
  EXPECT_NE(bytes.find("\"viewer_link_name\":"), std::string::npos);
  EXPECT_NE(bytes.find("\"link_base\""), std::string::npos);
  EXPECT_NE(bytes.find("\"source_group_name\":\"lite6\""), std::string::npos);
  EXPECT_NE(bytes.find("\"commandable_source_joint_names\":[\"joint1\",\"joint2\","
                       "\"joint3\",\"joint4\",\"joint5\",\"joint6\"]"),
            std::string::npos);
  EXPECT_NE(bytes.find("\"source_group_state_name\":\"home\""), std::string::npos);
  EXPECT_NE(bytes.find("\"source_end_effector_name\":\"lite_gripper\""), std::string::npos);
  EXPECT_NE(bytes.find("\"source_parent_link_name\":\"link_tcp\""), std::string::npos);
  EXPECT_NE(bytes.find("\"target_id\":\"arm\""), std::string::npos);
  EXPECT_NE(bytes.find("\"components\":[{\"component_id\":\"lite6_arm\",\"mounted_on\":null}]"),
            std::string::npos);
  EXPECT_EQ(bytes.find("compiled_"), std::string::npos);
  EXPECT_EQ(bytes.find("resource_id"), std::string::npos);
  EXPECT_EQ(bytes.find("control_binding"), std::string::npos);
  EXPECT_EQ(bytes.find("namespace"), std::string::npos);
}

// B6: one row per graph component, sorted by id; a mounted tool names its
// parent component and interface.
TEST(SourceModelProjection, ComponentRowsNameEachMountParent) {
  const auto component = [](std::string id) {
    return core::ControllableComponentDTO{core::ControllableComponentId(std::move(id)),
                                          core::ComponentTypeId("type"),
                                          core::ComponentTypeRevision("revision.type"),
                                          {}, {}, {}};
  };
  core::CellGraphDTO graph{
      "manyforge.cell_graph.v1",
      core::GraphRevision(
          "sha256:0000000000000000000000000000000000000000000000000000000000000000"),
      core::CellId("tool-split-cell"),
      {}, {}, {}, {}, {}, {}};
  graph.controllable_components = {component("lite_gripper"), component("lite6_robot"),
                                    component("io_box")};
  graph.component_attachments = {core::ComponentAttachmentEdgeDTO{
      core::ComponentAttachmentId("lite_gripper"),
      core::ComponentAttachmentRevision(
          "sha256:0000000000000000000000000000000000000000000000000000000000000000"),
      {core::ControllableComponentId("lite6_robot"), core::AttachmentInterfaceId("flange")},
      {core::ControllableComponentId("lite_gripper"), core::AttachmentInterfaceId("mount")},
      {}}};

  EXPECT_EQ(content_identity::canonical_json(SourceModelComponents(graph)),
            "[{\"component_id\":\"io_box\",\"mounted_on\":null},"
            "{\"component_id\":\"lite6_robot\",\"mounted_on\":null},"
            "{\"component_id\":\"lite_gripper\",\"mounted_on\":"
            "{\"component_id\":\"lite6_robot\",\"interface_id\":\"flange\"}}]");
}

}  // namespace
}  // namespace manyforge::headless
