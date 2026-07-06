// Unit tests for CollisionWorldSnapshotBuilder: invariants,
// determinism (sort + hash), sensitivity, count accuracy, mesh flag,
// shared_ptr wrapping.

#include <manyforge_core/collision/collision_world_snapshot_builder.hpp>

#include <gtest/gtest.h>

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

CollisionWorldBody make_robot_link(const std::string& link_name,
                                   double pose_x = 0.0) {
  CollisionBodyId body_id =
      "robot/r1/link/" + link_name + "/collision/0";
  CollisionWorldBody b;
  b.body_id = body_id;
  b.owner_kind = CollisionOwnerKind::kRobotLink;
  b.frame_id = "world";
  b.pose = Pose3DTO{{pose_x, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
  b.geometries.push_back(make_box_geom(body_id, CollisionOwnerKind::kRobotLink));
  return b;
}

CollisionWorldBody make_attached_object(const std::string& object_id,
                                        const std::string& parent_link) {
  CollisionBodyId body_id =
      "robot/r1/attached/" + object_id + "/shape/0";
  CollisionWorldBody b;
  b.body_id = body_id;
  b.owner_kind = CollisionOwnerKind::kAttachedObject;
  b.parent_link = parent_link;
  b.frame_id = parent_link;
  b.pose = Pose3DTO{{0.1, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
  b.geometries.push_back(
      make_box_geom(body_id, CollisionOwnerKind::kAttachedObject));
  return b;
}

CollisionWorldBody make_detached_object(const std::string& object_id) {
  CollisionBodyId body_id = "object/" + object_id + "/shape/0";
  CollisionWorldBody b;
  b.body_id = body_id;
  b.owner_kind = CollisionOwnerKind::kDetachedObject;
  b.frame_id = "world";
  b.pose = Pose3DTO{{1.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
  b.geometries.push_back(
      make_box_geom(body_id, CollisionOwnerKind::kDetachedObject));
  return b;
}

CollisionWorldBody make_fixture(const std::string& fixture_id) {
  CollisionBodyId body_id = "fixture/" + fixture_id + "/shape/0";
  CollisionWorldBody b;
  b.body_id = body_id;
  b.owner_kind = CollisionOwnerKind::kFixture;
  b.frame_id = "world";
  b.pose = Pose3DTO{{2.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
  b.geometries.push_back(make_box_geom(body_id, CollisionOwnerKind::kFixture));
  return b;
}

CollisionWorldSnapshotBuilder fresh_builder() {
  CollisionWorldSnapshotBuilder b;
  b.set_publisher_id("test_publisher").set_revision_id(1);
  return b;
}

CollisionRobotModelFrame make_robot_model_frame(double x = 0.0) {
  CollisionRobotModelFrame frame;
  frame.robot_id = "r1";
  frame.model_frame_id = "base_link";
  frame.world_pose = Pose3DTO{{x, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
  return frame;
}

// --- Happy path ---

TEST(CollisionWorldSnapshotBuilder, BuildsMinimalSnapshot) {
  auto result =
      fresh_builder().add_body(make_robot_link("link1")).build();
  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto snap = result.value();
  EXPECT_EQ(snap.publisher_id, "test_publisher");
  EXPECT_EQ(snap.revision.revision_id, 1u);
  EXPECT_FALSE(snap.revision.content_hash.empty());
  EXPECT_EQ(snap.body_count, 1);
  EXPECT_TRUE(snap.robot_model_frames.empty());
  EXPECT_EQ(snap.geometry_count, 1);
  EXPECT_EQ(snap.robot_link_count, 1);
  EXPECT_FALSE(snap.contains_mesh_geometry);
  EXPECT_TRUE(snap.all_bodies_have_geometry);
  EXPECT_FALSE(snap.bodies.front().body_content_hash.empty());
}

TEST(CollisionWorldSnapshotBuilder, CarriesSortedRobotModelFrames) {
  CollisionRobotModelFrame b;
  b.robot_id = "robot_b";
  b.model_frame_id = "base_link";
  b.world_pose = Pose3DTO{{0.2, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
  CollisionRobotModelFrame a;
  a.robot_id = "robot_a";
  a.model_frame_id = "base_link";
  a.world_pose = Pose3DTO{{0.1, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};

  auto result = fresh_builder()
                    .add_robot_model_frame(std::move(b))
                    .add_robot_model_frame(std::move(a))
                    .add_body(make_robot_link("link1"))
                    .build();
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_EQ(result.value().robot_model_frames.size(), 2u);
  EXPECT_EQ(result.value().robot_model_frames[0].robot_id, "robot_a");
  EXPECT_EQ(result.value().robot_model_frames[1].robot_id, "robot_b");
}

TEST(CollisionWorldSnapshotBuilder, CountsByOwnerKind) {
  auto result = fresh_builder()
                    .add_body(make_robot_link("link1"))
                    .add_body(make_robot_link("link2"))
                    .add_body(make_attached_object("gripper_load",
                                                   "robot/r1/link/tcp"))
                    .add_body(make_detached_object("crate_a"))
                    .add_body(make_fixture("wall"))
                    .build();
  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto& s = result.value();
  EXPECT_EQ(s.body_count, 5);
  EXPECT_EQ(s.geometry_count, 5);
  EXPECT_EQ(s.robot_link_count, 2);
  EXPECT_EQ(s.attached_object_count, 1);
  EXPECT_EQ(s.detached_object_count, 1);
  EXPECT_EQ(s.fixture_count, 1);
}

TEST(CollisionWorldSnapshotBuilder, BodiesAreSortedByBodyId) {
  auto result = fresh_builder()
                    .add_body(make_fixture("z_last"))
                    .add_body(make_robot_link("a_first"))
                    .add_body(make_robot_link("m_middle"))
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
                .add_body(make_robot_link("link1"))
                .add_body(make_robot_link("link2"))
                .build();
  auto r2 = fresh_builder()
                .add_body(make_robot_link("link1"))
                .add_body(make_robot_link("link2"))
                .build();
  ASSERT_TRUE(r1.ok());
  ASSERT_TRUE(r2.ok());
  EXPECT_EQ(r1.value().revision.content_hash, r2.value().revision.content_hash);
}

TEST(CollisionWorldSnapshotBuilder, ContentHashIsOrderInvariantInAddBody) {
  auto r_ascending = fresh_builder()
                         .add_body(make_robot_link("link1"))
                         .add_body(make_robot_link("link2"))
                         .add_body(make_robot_link("link3"))
                         .build();
  auto r_shuffled = fresh_builder()
                        .add_body(make_robot_link("link3"))
                        .add_body(make_robot_link("link1"))
                        .add_body(make_robot_link("link2"))
                        .build();
  ASSERT_TRUE(r_ascending.ok());
  ASSERT_TRUE(r_shuffled.ok());
  EXPECT_EQ(r_ascending.value().revision.content_hash,
            r_shuffled.value().revision.content_hash);
}

TEST(CollisionWorldSnapshotBuilder, ContentHashChangesWhenPoseChanges) {
  auto r1 = fresh_builder().add_body(make_robot_link("link1", 0.0)).build();
  auto r2 = fresh_builder().add_body(make_robot_link("link1", 0.1)).build();
  ASSERT_TRUE(r1.ok());
  ASSERT_TRUE(r2.ok());
  EXPECT_NE(r1.value().revision.content_hash, r2.value().revision.content_hash);
}

TEST(CollisionWorldSnapshotBuilder,
     ContentHashChangesWhenRobotModelFramePoseChanges) {
  auto r1 = fresh_builder()
                .add_robot_model_frame(make_robot_model_frame(0.0))
                .add_body(make_robot_link("link1"))
                .build();
  auto r2 = fresh_builder()
                .add_robot_model_frame(make_robot_model_frame(0.1))
                .add_body(make_robot_link("link1"))
                .build();
  ASSERT_TRUE(r1.ok());
  ASSERT_TRUE(r2.ok());
  EXPECT_NE(r1.value().revision.content_hash,
            r2.value().revision.content_hash);
}

TEST(CollisionWorldSnapshotBuilder, ContentHashChangesWhenBoxDimensionsChange) {
  CollisionWorldBody a = make_robot_link("link1");
  CollisionWorldBody b = make_robot_link("link1");
  b.geometries.front().box_dimensions_m = Vector3DTO{0.2, 0.1, 0.1};

  auto r1 = fresh_builder().add_body(std::move(a)).build();
  auto r2 = fresh_builder().add_body(std::move(b)).build();
  ASSERT_TRUE(r1.ok());
  ASSERT_TRUE(r2.ok());
  EXPECT_NE(r1.value().revision.content_hash, r2.value().revision.content_hash);
}

TEST(CollisionWorldSnapshotBuilder, SourceTimestampDoesNotChangeContentHash) {
  CollisionWorldBody a = make_robot_link("link1");
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
  CollisionWorldBody a = make_robot_link("link1");
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

TEST(CollisionWorldSnapshotBuilder, ReusesPrecomputedBodyContentHash) {
  CollisionWorldBody body = make_robot_link("link1");
  const auto body_hash = compute_collision_world_body_content_hash(body);
  body.body_content_hash = body_hash;

  auto result = fresh_builder().add_body(std::move(body)).build();

  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_EQ(result.value().bodies.size(), 1u);
  EXPECT_EQ(result.value().bodies.front().body_content_hash, body_hash);
}

TEST(CollisionWorldSnapshotBuilder, ContentHashChangesWhenMeshScaleChanges) {
  CollisionWorldBody a = make_robot_link("mesh_link");
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
               .add_body(make_robot_link("link1"))
               .build();
  auto b = CollisionWorldSnapshotBuilder()
               .set_publisher_id("pub_b")
               .set_revision_id(1)
               .add_body(make_robot_link("link1"))
               .build();
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  EXPECT_NE(a.value().revision.content_hash, b.value().revision.content_hash);
}

TEST(CollisionWorldSnapshotBuilder, RevisionIdDoesNotChangeContentHash) {
  auto a = CollisionWorldSnapshotBuilder()
               .set_publisher_id("pub")
               .set_revision_id(1)
               .add_body(make_robot_link("link1"))
               .build();
  auto b = CollisionWorldSnapshotBuilder()
               .set_publisher_id("pub")
               .set_revision_id(2)
               .add_body(make_robot_link("link1"))
               .build();
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(a.value().revision.content_hash, b.value().revision.content_hash);
  EXPECT_NE(a.value().revision.revision_id, b.value().revision.revision_id);
}

// --- Mesh-geometry flag ---

TEST(CollisionWorldSnapshotBuilder, ContainsMeshGeometryFiresOnTriangleMesh) {
  CollisionWorldBody body = make_robot_link("mesh_link");
  body.geometries.front().geometry_kind = CollisionGeometryKind::kTriangleMesh;
  body.geometries.front().box_dimensions_m.reset();
  body.geometries.front().source_hash = "triangle-mesh";
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(result.value().contains_mesh_geometry);
}

TEST(CollisionWorldSnapshotBuilder, ContainsMeshGeometryFiresOnCompound) {
  CollisionWorldBody body = make_robot_link("compound_link");
  body.geometries.front().geometry_kind = CollisionGeometryKind::kCompound;
  body.geometries.front().box_dimensions_m.reset();
  body.geometries.front().source_hash = "compound";
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(result.value().contains_mesh_geometry);
}

TEST(CollisionWorldSnapshotBuilder,
     ContainsMeshGeometryStaysFalseForPrimitivesOnly) {
  auto result = fresh_builder()
                    .add_body(make_robot_link("link_a"))
                    .add_body(make_fixture("wall"))
                    .build();
  ASSERT_TRUE(result.ok());
  EXPECT_FALSE(result.value().contains_mesh_geometry);
}

// --- Invariant violations ---

TEST(CollisionWorldSnapshotBuilder, RejectsEmptyPublisherId) {
  auto result = CollisionWorldSnapshotBuilder()
                    .set_revision_id(1)
                    .add_body(make_robot_link("link1"))
                    .build();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "invariant_violation");
}

TEST(CollisionWorldSnapshotBuilder, RejectsZeroRevisionId) {
  auto result = CollisionWorldSnapshotBuilder()
                    .set_publisher_id("pub")
                    .add_body(make_robot_link("link1"))
                    .build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsZeroSnapshotFormatVersion) {
  auto result = fresh_builder()
                    .set_snapshot_format_version(0)
                    .add_body(make_robot_link("link1"))
                    .build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsNegativeEmissionTime) {
  auto result = fresh_builder()
                    .set_emitted_at_ns(-1)
                    .add_body(make_robot_link("link1"))
                    .build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsBodyFreshnessBudgetWithoutTimestamp) {
  CollisionWorldBody body = make_robot_link("link1");
  body.source_freshness_budget_ns = 50'000'000;
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "invariant_violation");
}

TEST(CollisionWorldSnapshotBuilder, RejectsNegativeBodySourceTimestamp) {
  CollisionWorldBody body = make_robot_link("link1");
  body.source_timestamp_ns = -1;
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "invariant_violation");
}

TEST(CollisionWorldSnapshotBuilder, RejectsNonPositiveBodyFreshnessBudget) {
  CollisionWorldBody body = make_robot_link("link1");
  body.source_timestamp_ns = 100;
  body.source_freshness_budget_ns = 0;
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "invariant_violation");
}

TEST(CollisionWorldSnapshotBuilder, RejectsInvalidRobotModelFrame) {
  CollisionRobotModelFrame invalid = make_robot_model_frame();
  invalid.model_frame_id.clear();
  auto result = fresh_builder()
                    .add_robot_model_frame(std::move(invalid))
                    .add_body(make_robot_link("link1"))
                    .build();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "invariant_violation");
}

TEST(CollisionWorldSnapshotBuilder, RejectsDuplicateRobotModelFrame) {
  auto result = fresh_builder()
                    .add_robot_model_frame(make_robot_model_frame())
                    .add_robot_model_frame(make_robot_model_frame())
                    .add_body(make_robot_link("link1"))
                    .build();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "invariant_violation");
}

TEST(CollisionWorldSnapshotBuilder, RejectsDuplicateBodyIds) {
  auto result = fresh_builder()
                    .add_body(make_robot_link("link1"))
                    .add_body(make_robot_link("link1"))
                    .build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsEmptyBodyId) {
  CollisionWorldBody empty;
  empty.body_id = "";
  empty.owner_kind = CollisionOwnerKind::kFixture;
  empty.frame_id = "world";
  empty.geometries.push_back(
      make_box_geom("", CollisionOwnerKind::kFixture));
  auto result = fresh_builder().add_body(std::move(empty)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsBodyIdThatDoesNotMatchOwnerKind) {
  CollisionWorldBody body = make_robot_link("link1");
  body.body_id = "object/link1/shape/0";
  body.geometries.front().body_id = body.body_id;
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsEmptyFrameId) {
  CollisionWorldBody body = make_robot_link("link1");
  body.frame_id.clear();
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsBodyWithoutGeometries) {
  CollisionWorldBody empty;
  empty.body_id = "fixture/wall/shape/0";
  empty.owner_kind = CollisionOwnerKind::kFixture;
  empty.frame_id = "world";
  // No geometries pushed.
  auto result = fresh_builder().add_body(std::move(empty)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsGeometryBodyIdMismatch) {
  CollisionWorldBody body = make_robot_link("link1");
  body.geometries.front().body_id = "fixture/wrong_id/shape/0";
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsGeometryOwnerKindMismatch) {
  CollisionWorldBody body = make_robot_link("link1");
  body.geometries.front().owner_kind = CollisionOwnerKind::kFixture;
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsBoxWithoutDimensions) {
  CollisionWorldBody body = make_robot_link("link1");
  body.geometries.front().box_dimensions_m.reset();
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsNonPositivePrimitiveDimensions) {
  CollisionWorldBody body = make_robot_link("link1");
  body.geometries.front().box_dimensions_m = Vector3DTO{0.1, 0.0, 0.1};
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsUnrelatedPrimitiveDimensions) {
  CollisionWorldBody body = make_robot_link("link1");
  body.geometries.front().sphere_radius_m = 0.1;
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsPrimitiveWithMeshScale) {
  CollisionWorldBody body = make_robot_link("link1");
  body.geometries.front().mesh_scale = Vector3DTO{1.0, 1.0, 1.0};
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsMeshWithoutSourceIdentity) {
  CollisionWorldBody body = make_robot_link("mesh_link");
  body.geometries.front().geometry_kind = CollisionGeometryKind::kTriangleMesh;
  body.geometries.front().box_dimensions_m.reset();
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, AcceptsMeshWithSourceHash) {
  CollisionWorldBody body = make_robot_link("mesh_link");
  body.geometries.front().geometry_kind = CollisionGeometryKind::kTriangleMesh;
  body.geometries.front().box_dimensions_m.reset();
  body.geometries.front().source_hash = "mesh-hash";
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_TRUE(result.value().contains_mesh_geometry);
}

TEST(CollisionWorldSnapshotBuilder, RejectsMeshWithInvalidScale) {
  CollisionWorldBody body = make_robot_link("mesh_link");
  body.geometries.front().geometry_kind = CollisionGeometryKind::kTriangleMesh;
  body.geometries.front().box_dimensions_m.reset();
  body.geometries.front().source_hash = "mesh-hash";
  body.geometries.front().mesh_scale = Vector3DTO{1.0, 0.0, 1.0};
  auto result = fresh_builder().add_body(std::move(body)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsAttachedWithoutParentLink) {
  CollisionWorldBody attached =
      make_attached_object("load", "robot/r1/link/tcp");
  attached.parent_link.reset();
  auto result = fresh_builder().add_body(std::move(attached)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsAttachedWithNonRobotLinkParent) {
  CollisionWorldBody attached =
      make_attached_object("load", "robot/r1/link/tcp");
  attached.parent_link = "object/not_a_link/shape/0";
  auto result = fresh_builder().add_body(std::move(attached)).build();
  ASSERT_FALSE(result.ok());
}

TEST(CollisionWorldSnapshotBuilder, RejectsNonAttachedWithParentLink) {
  CollisionWorldBody fixture = make_fixture("wall");
  fixture.parent_link = "robot/r1/link/tcp";
  auto result = fresh_builder().add_body(std::move(fixture)).build();
  ASSERT_FALSE(result.ok());
}

// --- shared_ptr wrapper ---

TEST(CollisionWorldSnapshotBuilder, BuildSharedReturnsImmutableSharedPtr) {
  auto result = fresh_builder()
                    .add_body(make_robot_link("link1"))
                    .build_shared();
  ASSERT_TRUE(result.ok());
  CollisionWorldSnapshotPtr ptr = result.value();
  ASSERT_TRUE(ptr);
  EXPECT_EQ(ptr->body_count, 1);
  // shared_ptr<const CollisionWorldSnapshot> — type cannot be mutated.
  static_assert(
      std::is_const_v<typename CollisionWorldSnapshotPtr::element_type>,
      "CollisionWorldSnapshotPtr must point to const snapshot");
}

TEST(CollisionWorldSnapshotBuilder, BuildSharedPropagatesError) {
  auto result =
      CollisionWorldSnapshotBuilder().set_revision_id(1).build_shared();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "invariant_violation");
}

// --- Per-body content hash ---

TEST(CollisionWorldSnapshotBuilder,
     PerBodyContentHashChangesIndependentlyOfOtherBodies) {
  auto r1 = fresh_builder()
                .add_body(make_robot_link("link_a"))
                .add_body(make_robot_link("link_b"))
                .build();
  auto r2 = fresh_builder()
                .add_body(make_robot_link("link_a"))
                .add_body(make_robot_link("link_b", 0.5))  // moved
                .build();
  ASSERT_TRUE(r1.ok());
  ASSERT_TRUE(r2.ok());
  ASSERT_EQ(r1.value().bodies.size(), 2u);
  ASSERT_EQ(r2.value().bodies.size(), 2u);
  // link_a unchanged → same per-body hash.
  EXPECT_EQ(r1.value().bodies[0].body_content_hash,
            r2.value().bodies[0].body_content_hash);
  // link_b moved → different per-body hash.
  EXPECT_NE(r1.value().bodies[1].body_content_hash,
            r2.value().bodies[1].body_content_hash);
  // World hash differs because link_b changed.
  EXPECT_NE(r1.value().revision.content_hash,
            r2.value().revision.content_hash);
}

}  // namespace
}  // namespace manyforge::core
