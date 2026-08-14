// Unit tests for CollisionWorldSnapshotBuilder: invariants,
// determinism (sort + hash), sensitivity, count accuracy, mesh flag,
// shared_ptr wrapping.

#include <gtest/gtest.h>

#include <manyforge_core/collision/collision_world_snapshot_builder.hpp>
#include <manyforge_core/execution_collision_support.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

CollisionGeometryDescriptor make_box_geom(const CollisionBodyId& body_id,
                                          CollisionOwnerKind owner_kind) {
  CollisionGeometryDescriptor g;
  g.body_id = body_id;
  g.owner_kind = owner_kind;
  g.geometry_kind = CollisionGeometryKind::kBox;
  g.local_pose = Pose3DTO{{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
  g.box_dimensions_m = Vector3DTO{0.1, 0.1, 0.1};
  return g;
}

CollisionWorldBody make_component_body(const std::string& body_role, double pose_x = 0.0) {
  CollisionBodyId body_id = "component/arm/model/arm_model/body/" + body_role + "/collision/0";
  CollisionWorldBody b;
  b.body_id = body_id;
  b.owner_kind = CollisionOwnerKind::kComponentModelBody;
  b.frame_id = "compiled_" + body_role;
  b.pose = Pose3DTO{{pose_x, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
  b.geometries.push_back(make_box_geom(body_id, CollisionOwnerKind::kComponentModelBody));
  return b;
}

CollisionWorldBody make_carried_scene_resource(const std::string& resource_id,
                                               const CollisionBodyId& parent_body_id) {
  CollisionBodyId body_id = "scene/" + resource_id + "/shape/0";
  CollisionWorldBody b;
  b.body_id = body_id;
  b.owner_kind = CollisionOwnerKind::kSceneResource;
  b.parent_body_id = parent_body_id;
  b.frame_id = parent_body_id;
  b.pose = Pose3DTO{{0.1, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
  b.geometries.push_back(make_box_geom(body_id, CollisionOwnerKind::kSceneResource));
  return b;
}

CollisionWorldBody make_scene_resource(const std::string& resource_id, double pose_x = 1.0) {
  CollisionBodyId body_id = "scene/" + resource_id + "/shape/0";
  CollisionWorldBody b;
  b.body_id = body_id;
  b.owner_kind = CollisionOwnerKind::kSceneResource;
  b.frame_id = "world";
  b.pose = Pose3DTO{{pose_x, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
  b.geometries.push_back(make_box_geom(body_id, CollisionOwnerKind::kSceneResource));
  return b;
}

CollisionWorldBody make_fixture(const std::string& fixture_id) {
  return make_scene_resource(fixture_id, 2.0);
}

CollisionWorldBody make_runtime_object(const std::string& object_id) {
  CollisionBodyId body_id = "runtime_object/" + object_id + "/shape/0";
  CollisionWorldBody b;
  b.body_id = body_id;
  b.owner_kind = CollisionOwnerKind::kRuntimeObject;
  b.frame_id = "world";
  b.pose = Pose3DTO{{3.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
  b.geometries.push_back(make_box_geom(body_id, CollisionOwnerKind::kRuntimeObject));
  return b;
}

CollisionWorldSnapshotBuilder fresh_builder() {
  CollisionWorldSnapshotBuilder b;
  b.set_publisher_id("test_publisher").set_revision_id(1);
  return b;
}

CollisionComponentModelFrame make_component_model_frame(double x = 0.0) {
  return CollisionComponentModelFrame{ControllableComponentId{"arm"},
                                      ModelInstanceId{"arm_model"},
                                      FrameRoleId{"base"},
                                      "base_link",
                                      ModelRevision{"model-revision-1"},
                                      Pose3DTO{{x, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}}};
}

// --- Happy path ---

TEST(CollisionWorldSnapshotBuilder, BuildsMinimalSnapshot) {
  auto result = fresh_builder().add_body(make_component_body("link1")).build();
  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto snap = result.value();
  EXPECT_EQ(snap.publisher_id, "test_publisher");
  EXPECT_EQ(snap.revision.revision_id, 1u);
  EXPECT_TRUE(IsExactLowercaseSha256Digest(snap.revision.content_hash));
  EXPECT_EQ(snap.body_count, 1);
  EXPECT_TRUE(snap.component_model_frames.empty());
  EXPECT_EQ(snap.geometry_count, 1);
  EXPECT_EQ(snap.component_model_body_count, 1);
  EXPECT_FALSE(snap.contains_mesh_geometry);
  EXPECT_TRUE(snap.all_bodies_have_geometry);
  EXPECT_TRUE(IsExactLowercaseSha256Digest(snap.bodies.front().body_content_hash));
}

TEST(CollisionWorldSnapshotBuilder, CarriesSortedComponentModelFrames) {
  CollisionComponentModelFrame b{ControllableComponentId{"component_b"},
                                 ModelInstanceId{"model_b"},
                                 FrameRoleId{"base"},
                                 "component_b_base",
                                 ModelRevision{"revision-b"},
                                 Pose3DTO{{0.2, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}}};
  CollisionComponentModelFrame a{ControllableComponentId{"component_a"},
                                 ModelInstanceId{"model_a"},
                                 FrameRoleId{"base"},
                                 "component_a_base",
                                 ModelRevision{"revision-a"},
                                 Pose3DTO{{0.1, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}}};

  auto result = fresh_builder()
                    .add_component_model_frame(std::move(b))
                    .add_component_model_frame(std::move(a))
                    .add_body(make_component_body("link1"))
                    .build();
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_EQ(result.value().component_model_frames.size(), 2u);
  EXPECT_EQ(result.value().component_model_frames[0].controllable_component_id.value(),
            "component_a");
  EXPECT_EQ(result.value().component_model_frames[1].controllable_component_id.value(),
            "component_b");
}

TEST(CollisionWorldSnapshotBuilder, CountsByOwnerKind) {
  auto result = fresh_builder()
                    .add_body(make_component_body("link1"))
                    .add_body(make_component_body("link2"))
                    .add_body(make_carried_scene_resource(
                        "gripper_load", "component/arm/model/arm_model/body/link1/collision/0"))
                    .add_body(make_scene_resource("crate_a"))
                    .add_body(make_fixture("wall"))
                    .add_body(make_runtime_object("detected_person"))
                    .build();
  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto& s = result.value();
  EXPECT_EQ(s.body_count, 6);
  EXPECT_EQ(s.geometry_count, 6);
  EXPECT_EQ(s.component_model_body_count, 2);
  EXPECT_EQ(s.scene_resource_count, 3);
  EXPECT_EQ(s.runtime_object_count, 1);
}

TEST(CollisionWorldSnapshotBuilder, BodiesAreSortedByBodyId) {
  auto result = fresh_builder()
                    .add_body(make_fixture("z_last"))
                    .add_body(make_component_body("a_first"))
                    .add_body(make_component_body("m_middle"))
                    .build();
  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto& bodies = result.value().bodies;
  ASSERT_EQ(bodies.size(), 3u);
  EXPECT_LT(bodies[0].body_id, bodies[1].body_id);
  EXPECT_LT(bodies[1].body_id, bodies[2].body_id);
}

// --- Determinism ---

TEST(CollisionWorldSnapshotBuilder, ContentHashIsDeterministicForIdenticalInput) {
  auto r1 = fresh_builder()
                .add_body(make_component_body("link1"))
                .add_body(make_component_body("link2"))
                .build();
  auto r2 = fresh_builder()
                .add_body(make_component_body("link1"))
                .add_body(make_component_body("link2"))
                .build();
  ASSERT_TRUE(r1.ok());
  ASSERT_TRUE(r2.ok());
  EXPECT_EQ(r1.value().revision.content_hash, r2.value().revision.content_hash);
}

TEST(CollisionWorldSnapshotBuilder, ContentHashIsOrderInvariantInAddBody) {
  auto r_ascending = fresh_builder()
                         .add_body(make_component_body("link1"))
                         .add_body(make_component_body("link2"))
                         .add_body(make_component_body("link3"))
                         .build();
  auto r_shuffled = fresh_builder()
                        .add_body(make_component_body("link3"))
                        .add_body(make_component_body("link1"))
                        .add_body(make_component_body("link2"))
                        .build();
  ASSERT_TRUE(r_ascending.ok());
  ASSERT_TRUE(r_shuffled.ok());
  EXPECT_EQ(r_ascending.value().revision.content_hash, r_shuffled.value().revision.content_hash);
}

TEST(CollisionWorldSnapshotBuilder, ContentHashChangesWhenPoseChanges) {
  auto r1 = fresh_builder().add_body(make_component_body("link1", 0.0)).build();
  auto r2 = fresh_builder().add_body(make_component_body("link1", 0.1)).build();
  ASSERT_TRUE(r1.ok());
  ASSERT_TRUE(r2.ok());
  EXPECT_NE(r1.value().revision.content_hash, r2.value().revision.content_hash);
}

TEST(CollisionWorldSnapshotBuilder, ContentHashChangesWhenComponentModelFramePoseChanges) {
  auto r1 = fresh_builder()
                .add_component_model_frame(make_component_model_frame(0.0))
                .add_body(make_component_body("link1"))
                .build();
  auto r2 = fresh_builder()
                .add_component_model_frame(make_component_model_frame(0.1))
                .add_body(make_component_body("link1"))
                .build();
  ASSERT_TRUE(r1.ok());
  ASSERT_TRUE(r2.ok());
  EXPECT_NE(r1.value().revision.content_hash, r2.value().revision.content_hash);
}

TEST(CollisionWorldSnapshotBuilder, ContentHashChangesWhenBoxDimensionsChange) {
  CollisionWorldBody a = make_component_body("link1");
  CollisionWorldBody b = make_component_body("link1");
  b.geometries.front().box_dimensions_m = Vector3DTO{0.2, 0.1, 0.1};

  auto r1 = fresh_builder().add_body(std::move(a)).build();
  auto r2 = fresh_builder().add_body(std::move(b)).build();
  ASSERT_TRUE(r1.ok());
  ASSERT_TRUE(r2.ok());
  EXPECT_NE(r1.value().revision.content_hash, r2.value().revision.content_hash);
}

TEST(CollisionWorldSnapshotBuilder, SourceTimestampDoesNotChangeContentHash) {
  CollisionWorldBody a = make_component_body("link1");
  a.source_timestamp_ns = 100;
  a.source_freshness_budget_ns = 50'000'000;
  CollisionWorldBody b = a;
  b.source_timestamp_ns = 200;

  auto r1 = fresh_builder().add_body(std::move(a)).build();
  auto r2 = fresh_builder().add_body(std::move(b)).build();
  ASSERT_TRUE(r1.ok());
  ASSERT_TRUE(r2.ok());
  EXPECT_EQ(r1.value().revision.content_hash, r2.value().revision.content_hash);
}

TEST(CollisionWorldSnapshotBuilder, ContentHashChangesWhenFreshnessBudgetChanges) {
  CollisionWorldBody a = make_component_body("link1");
  a.source_timestamp_ns = 100;
  a.source_freshness_budget_ns = 50'000'000;
  CollisionWorldBody b = a;
  b.source_freshness_budget_ns = 100'000'000;

  auto r1 = fresh_builder().add_body(std::move(a)).build();
  auto r2 = fresh_builder().add_body(std::move(b)).build();
  ASSERT_TRUE(r1.ok());
  ASSERT_TRUE(r2.ok());
  EXPECT_NE(r1.value().revision.content_hash, r2.value().revision.content_hash);
}

TEST(CollisionWorldSnapshotBuilder, RecomputesCallerProvidedBodyContentHash) {
  CollisionWorldBody body = make_component_body("link1");
  const auto body_hash = compute_collision_world_body_content_hash(body);
  body.body_content_hash = "forged";

  auto result = fresh_builder().add_body(std::move(body)).build();

  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_EQ(result.value().bodies.size(), 1u);
  EXPECT_EQ(result.value().bodies.front().body_content_hash, body_hash);
}

TEST(CollisionWorldSnapshotBuilder, RejectsInvalidPolicyOverlayDigest) {
  auto result = fresh_builder()
                    .set_policy_overlay_content_hash("legacy-policy-hash")
                    .add_body(make_component_body("link1"))
                    .build();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "invariant_violation");
}

TEST(CollisionWorldSnapshotBuilder, ContentHashChangesWhenMeshScaleChanges) {
  CollisionWorldBody a = make_component_body("mesh_link");
  a.geometries.front().geometry_kind = CollisionGeometryKind::kTriangleMesh;
  a.geometries.front().box_dimensions_m.reset();
  a.geometries.front().source_hash = "mesh-hash";
  a.geometries.front().mesh_scale = Vector3DTO{1.0, 1.0, 1.0};

  CollisionWorldBody b = a;
  b.geometries.front().mesh_scale = Vector3DTO{1.0, 1.5, 1.15};

  auto r1 = fresh_builder().add_body(std::move(a)).build();
  auto r2 = fresh_builder().add_body(std::move(b)).build();
  ASSERT_TRUE(r1.ok());
  ASSERT_TRUE(r2.ok());
  EXPECT_NE(r1.value().revision.content_hash, r2.value().revision.content_hash);
}

TEST(CollisionWorldSnapshotBuilder, ContentHashChangesWhenPublisherChanges) {
  auto a = CollisionWorldSnapshotBuilder()
               .set_publisher_id("pub_a")
               .set_revision_id(1)
               .add_body(make_component_body("link1"))
               .build();
  auto b = CollisionWorldSnapshotBuilder()
               .set_publisher_id("pub_b")
               .set_revision_id(1)
               .add_body(make_component_body("link1"))
               .build();
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  EXPECT_NE(a.value().revision.content_hash, b.value().revision.content_hash);
}

TEST(CollisionWorldSnapshotBuilder, RevisionIdDoesNotChangeContentHash) {
  auto a = CollisionWorldSnapshotBuilder()
               .set_publisher_id("pub")
               .set_revision_id(1)
               .add_body(make_component_body("link1"))
               .build();
  auto b = CollisionWorldSnapshotBuilder()
               .set_publisher_id("pub")
               .set_revision_id(2)
               .add_body(make_component_body("link1"))
               .build();
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(a.value().revision.content_hash, b.value().revision.content_hash);
  EXPECT_NE(a.value().revision.revision_id, b.value().revision.revision_id);
}

// --- Mesh-geometry flag ---

TEST(CollisionWorldSnapshotBuilder, ContainsMeshGeometryFiresOnTriangleMesh) {
  CollisionWorldBody body = make_component_body("mesh_link");
  body.geometries.front().geometry_kind = CollisionGeometryKind::kTriangleMesh;
  body.geometries.front().box_dimensions_m.reset();
  body.geometries.front().source_hash = "triangle-mesh";
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(result.value().contains_mesh_geometry);
}

TEST(CollisionWorldSnapshotBuilder, ContainsMeshGeometryFiresOnCompound) {
  CollisionWorldBody body = make_component_body("compound_link");
  body.geometries.front().geometry_kind = CollisionGeometryKind::kCompound;
  body.geometries.front().box_dimensions_m.reset();
  body.geometries.front().source_hash = "compound";
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(result.value().contains_mesh_geometry);
}

TEST(CollisionWorldSnapshotBuilder, ContainsMeshGeometryStaysFalseForPrimitivesOnly) {
  auto result = fresh_builder()
                    .add_body(make_component_body("link_a"))
                    .add_body(make_fixture("wall"))
                    .build();
  ASSERT_TRUE(result.ok());
  EXPECT_FALSE(result.value().contains_mesh_geometry);
}

// --- Invariant violations ---

TEST(CollisionWorldSnapshotBuilder, RejectsEmptyPublisherId) {
  auto result = CollisionWorldSnapshotBuilder()
                    .set_revision_id(1)
                    .add_body(make_component_body("link1"))
                    .build();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "invariant_violation");
}

TEST(CollisionWorldSnapshotBuilder, RejectsZeroRevisionId) {
  auto result = CollisionWorldSnapshotBuilder()
                    .set_publisher_id("pub")
                    .add_body(make_component_body("link1"))
                    .build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsZeroSnapshotFormatVersion) {
  auto result =
      fresh_builder().set_snapshot_format_version(0).add_body(make_component_body("link1")).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsNegativeEmissionTime) {
  auto result =
      fresh_builder().set_emitted_at_ns(-1).add_body(make_component_body("link1")).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsBodyFreshnessBudgetWithoutTimestamp) {
  CollisionWorldBody body = make_component_body("link1");
  body.source_freshness_budget_ns = 50'000'000;
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "invariant_violation");
}

TEST(CollisionWorldSnapshotBuilder, RejectsNegativeBodySourceTimestamp) {
  CollisionWorldBody body = make_component_body("link1");
  body.source_timestamp_ns = -1;
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "invariant_violation");
}

TEST(CollisionWorldSnapshotBuilder, RejectsNonPositiveBodyFreshnessBudget) {
  CollisionWorldBody body = make_component_body("link1");
  body.source_timestamp_ns = 100;
  body.source_freshness_budget_ns = 0;
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "invariant_violation");
}

TEST(CollisionWorldSnapshotBuilder, RejectsInvalidComponentModelFrame) {
  auto invalid = make_component_model_frame();
  invalid.compiled_frame_name.clear();
  auto result = fresh_builder()
                    .add_component_model_frame(std::move(invalid))
                    .add_body(make_component_body("link1"))
                    .build();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "invariant_violation");
}

TEST(CollisionWorldSnapshotBuilder, RejectsDuplicateComponentModelFrame) {
  auto result = fresh_builder()
                    .add_component_model_frame(make_component_model_frame())
                    .add_component_model_frame(make_component_model_frame())
                    .add_body(make_component_body("link1"))
                    .build();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "invariant_violation");
}

TEST(CollisionWorldSnapshotBuilder, RejectsDuplicateBodyIds) {
  auto result = fresh_builder()
                    .add_body(make_component_body("link1"))
                    .add_body(make_component_body("link1"))
                    .build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsEmptyBodyId) {
  CollisionWorldBody empty;
  empty.body_id = "";
  empty.owner_kind = CollisionOwnerKind::kSceneResource;
  empty.frame_id = "world";
  empty.geometries.push_back(make_box_geom("", CollisionOwnerKind::kSceneResource));
  auto result = fresh_builder().add_body(std::move(empty)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsBodyIdThatDoesNotMatchOwnerKind) {
  CollisionWorldBody body = make_component_body("link1");
  body.body_id = "scene/link1/shape/0";
  body.geometries.front().body_id = body.body_id;
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsEmptyFrameId) {
  CollisionWorldBody body = make_component_body("link1");
  body.frame_id.clear();
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsBodyWithoutGeometries) {
  CollisionWorldBody empty;
  empty.body_id = "scene/wall/shape/0";
  empty.owner_kind = CollisionOwnerKind::kSceneResource;
  empty.frame_id = "world";
  // No geometries pushed.
  auto result = fresh_builder().add_body(std::move(empty)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsGeometryBodyIdMismatch) {
  CollisionWorldBody body = make_component_body("link1");
  body.geometries.front().body_id = "scene/wrong_id/shape/0";
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsGeometryOwnerKindMismatch) {
  CollisionWorldBody body = make_component_body("link1");
  body.geometries.front().owner_kind = CollisionOwnerKind::kSceneResource;
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsBoxWithoutDimensions) {
  CollisionWorldBody body = make_component_body("link1");
  body.geometries.front().box_dimensions_m.reset();
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsNonPositivePrimitiveDimensions) {
  CollisionWorldBody body = make_component_body("link1");
  body.geometries.front().box_dimensions_m = Vector3DTO{0.1, 0.0, 0.1};
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsUnrelatedPrimitiveDimensions) {
  CollisionWorldBody body = make_component_body("link1");
  body.geometries.front().sphere_radius_m = 0.1;
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsPrimitiveWithMeshScale) {
  CollisionWorldBody body = make_component_body("link1");
  body.geometries.front().mesh_scale = Vector3DTO{1.0, 1.0, 1.0};
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsMeshWithoutSourceIdentity) {
  CollisionWorldBody body = make_component_body("mesh_link");
  body.geometries.front().geometry_kind = CollisionGeometryKind::kTriangleMesh;
  body.geometries.front().box_dimensions_m.reset();
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, AcceptsMeshWithSourceHash) {
  CollisionWorldBody body = make_component_body("mesh_link");
  body.geometries.front().geometry_kind = CollisionGeometryKind::kTriangleMesh;
  body.geometries.front().box_dimensions_m.reset();
  body.geometries.front().source_hash = "mesh-hash";
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_TRUE(result.value().contains_mesh_geometry);
}

TEST(CollisionWorldSnapshotBuilder, RejectsMeshWithInvalidScale) {
  CollisionWorldBody body = make_component_body("mesh_link");
  body.geometries.front().geometry_kind = CollisionGeometryKind::kTriangleMesh;
  body.geometries.front().box_dimensions_m.reset();
  body.geometries.front().source_hash = "mesh-hash";
  body.geometries.front().mesh_scale = Vector3DTO{1.0, 0.0, 1.0};
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, AcceptsDetachedSceneResourceWithoutParent) {
  auto result = fresh_builder().add_body(make_scene_resource("load")).build();
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_FALSE(result.value().bodies.front().parent_body_id.has_value());
}

TEST(CollisionWorldSnapshotBuilder, RejectsCarriedWithNonComponentBodyParent) {
  auto attached =
      make_carried_scene_resource("load", "component/arm/model/arm_model/body/tcp/collision/0");
  attached.parent_body_id = "scene/not_a_component_body/shape/0";
  auto result = fresh_builder().add_body(std::move(attached)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsCarriedWithMissingParentBody) {
  auto attached =
      make_carried_scene_resource("load", "component/arm/model/arm_model/body/tcp/collision/0");
  auto result = fresh_builder().add_body(std::move(attached)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsRuntimeObjectWithParent) {
  auto runtime_object = make_runtime_object("transient_obstacle");
  runtime_object.parent_body_id = "component/arm/model/arm_model/body/tcp/collision/0";
  auto result = fresh_builder().add_body(std::move(runtime_object)).build();
  ASSERT_FALSE(result.ok());
}

// --- shared_ptr wrapper ---

TEST(CollisionWorldSnapshotBuilder, BuildSharedReturnsImmutableSharedPtr) {
  auto result = fresh_builder().add_body(make_component_body("link1")).build_shared();
  ASSERT_TRUE(result.ok());
  CollisionWorldSnapshotPtr ptr = result.value();
  ASSERT_TRUE(ptr);
  EXPECT_EQ(ptr->body_count, 1);
  // shared_ptr<const CollisionWorldSnapshot> — type cannot be mutated.
  static_assert(std::is_const_v<typename CollisionWorldSnapshotPtr::element_type>,
                "CollisionWorldSnapshotPtr must point to const snapshot");
}

TEST(CollisionWorldSnapshotBuilder, BuildSharedPropagatesError) {
  auto result = CollisionWorldSnapshotBuilder().set_revision_id(1).build_shared();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "invariant_violation");
}

// --- Per-body content hash ---

TEST(CollisionWorldSnapshotBuilder, PerBodyContentHashChangesIndependentlyOfOtherBodies) {
  auto r1 = fresh_builder()
                .add_body(make_component_body("link_a"))
                .add_body(make_component_body("link_b"))
                .build();
  auto r2 = fresh_builder()
                .add_body(make_component_body("link_a"))
                .add_body(make_component_body("link_b", 0.5))  // moved
                .build();
  ASSERT_TRUE(r1.ok());
  ASSERT_TRUE(r2.ok());
  ASSERT_EQ(r1.value().bodies.size(), 2u);
  ASSERT_EQ(r2.value().bodies.size(), 2u);
  // link_a unchanged → same per-body hash.
  EXPECT_EQ(r1.value().bodies[0].body_content_hash, r2.value().bodies[0].body_content_hash);
  // link_b moved → different per-body hash.
  EXPECT_NE(r1.value().bodies[1].body_content_hash, r2.value().bodies[1].body_content_hash);
  // World hash differs because link_b changed.
  EXPECT_NE(r1.value().revision.content_hash, r2.value().revision.content_hash);
}

TEST(CollisionWorldSnapshotBuilder, RuntimeFreshnessSidecarOverridesSnapshotTimestamp) {
  auto body = make_scene_resource("dynamic_box");
  body.source_id = "vision";
  body.source_timestamp_ns = 0;
  body.source_freshness_budget_ns = 100;
  auto result = fresh_builder().add_body(body).build();
  ASSERT_TRUE(result.ok());

  CollisionWorldBodyFreshness freshness;
  freshness.body_id = body.body_id;
  freshness.source_id = "vision";
  freshness.source_timestamp_ns = 950;
  freshness.source_freshness_budget_ns = 100;
  std::vector<CollisionWorldBodyFreshness> sidecar{freshness};

  std::map<std::string, std::string> details;
  EXPECT_FALSE(
      validate_dynamic_body_freshness(result.value(), sidecar, 1'000, 1'000, details).has_value());
  EXPECT_EQ(details.at("dynamic_body_source_timestamp_ns"), "950");
}

TEST(CollisionWorldSnapshotBuilder, RuntimeFreshnessSidecarFailsClosedWhenUnsorted) {
  auto body_a = make_scene_resource("dynamic_box_a");
  body_a.source_id = "vision";
  body_a.source_timestamp_ns = 0;
  body_a.source_freshness_budget_ns = 100;
  auto body_b = make_scene_resource("dynamic_box_b");
  body_b.source_id = "vision";
  body_b.source_timestamp_ns = 0;
  body_b.source_freshness_budget_ns = 100;
  auto result = fresh_builder().add_body(body_a).add_body(body_b).build();
  ASSERT_TRUE(result.ok());

  CollisionWorldBodyFreshness freshness_b;
  freshness_b.body_id = body_b.body_id;
  freshness_b.source_id = "vision";
  freshness_b.source_timestamp_ns = 950;
  freshness_b.source_freshness_budget_ns = 100;
  CollisionWorldBodyFreshness freshness_a;
  freshness_a.body_id = body_a.body_id;
  freshness_a.source_id = "vision";
  freshness_a.source_timestamp_ns = 950;
  freshness_a.source_freshness_budget_ns = 100;
  std::vector<CollisionWorldBodyFreshness> sidecar{freshness_b, freshness_a};

  std::map<std::string, std::string> details;
  const auto reason =
      validate_dynamic_body_freshness(result.value(), sidecar, 1'000, 1'000, details);
  ASSERT_TRUE(reason.has_value());
  EXPECT_EQ(*reason, "dynamic_body_freshness_sidecar_not_sorted");
  EXPECT_EQ(details.at("dynamic_body_freshness_sidecar_status"), "unsorted");
}

}  // namespace
}  // namespace manyforge::core
