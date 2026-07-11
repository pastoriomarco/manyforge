// Unit tests for RoboPlanCollisionBodyIdMapper (Chunk 1 of the
// collision-ownership plan). These tests cover the parts of the mapper that
// do not require a live roboplan::Scene: id format helpers, lookup semantics,
// revision behavior, and deterministic content hashing.
//
// Integration with the dedicated diagnostic comparator and its
// scene-fingerprint-triggered rebuild is covered separately by tests that
// construct a real Scene.

#include <manyforge_roboplan/collision_body_id_mapper.hpp>
#include <manyforge_roboplan/roboplan_projection_backend.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using manyforge::core::CollisionShapeDTO;
using manyforge::core::BoxShapeDTO;
using manyforge::core::Pose3DTO;
using manyforge::roboplan_backend::RoboPlanCollisionBodyIdMapper;
using manyforge::roboplan_backend::RoboPlanProjectionBackend;

using Snapshot = RoboPlanProjectionBackend::SceneObjectSnapshot;

Snapshot make_object(const std::string& id, bool attached) {
  Snapshot s;
  s.object_id = id;
  s.shape = CollisionShapeDTO{BoxShapeDTO{}};
  s.frame_id = "universe";
  s.attached = attached;
  return s;
}

std::vector<std::pair<std::string, manyforge::core::CollisionBodyId>>
no_robot_mappings() {
  return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// Format helpers
// ---------------------------------------------------------------------------

TEST(CollisionBodyIdMapperFormat, RobotLinkId) {
  EXPECT_EQ(
      RoboPlanCollisionBodyIdMapper::make_robot_link_id("ur10e", "wrist_3_link", 0),
      "robot/ur10e/link/wrist_3_link/collision/0");
}

TEST(CollisionBodyIdMapperFormat, AttachedObjectId) {
  EXPECT_EQ(
      RoboPlanCollisionBodyIdMapper::make_attached_object_id("ur10e", "cheese_box", 0),
      "robot/ur10e/attached/cheese_box/shape/0");
}

TEST(CollisionBodyIdMapperFormat, DetachedObjectId) {
  EXPECT_EQ(
      RoboPlanCollisionBodyIdMapper::make_detached_object_id("conveyor_box", 0),
      "object/conveyor_box/shape/0");
}

TEST(CollisionBodyIdMapperFormat, FixtureId) {
  EXPECT_EQ(
      RoboPlanCollisionBodyIdMapper::make_fixture_id("table_top", 2),
      "fixture/table_top/shape/2");
}

// ---------------------------------------------------------------------------
// Initial state
// ---------------------------------------------------------------------------

TEST(CollisionBodyIdMapper, InitialRevisionIsZero) {
  RoboPlanCollisionBodyIdMapper mapper;
  EXPECT_EQ(mapper.current_revision().revision_id, 0u);
  EXPECT_TRUE(mapper.current_revision().content_hash.empty());
}

TEST(CollisionBodyIdMapper, LookupBeforeRebuildReturnsNullopt) {
  RoboPlanCollisionBodyIdMapper mapper;
  EXPECT_FALSE(mapper.lookup("anything").has_value());
}

// ---------------------------------------------------------------------------
// Rebuild + lookup
// ---------------------------------------------------------------------------

TEST(CollisionBodyIdMapper, RebuildPopulatesDetachedObjects) {
  RoboPlanCollisionBodyIdMapper mapper;
  std::vector<Snapshot> objs = {
      make_object("cheese_box", /*attached=*/false),
      make_object("conveyor_box", /*attached=*/false),
  };
  mapper.rebuild("ur10e", objs, no_robot_mappings());

  auto a = mapper.lookup("cheese_box");
  auto b = mapper.lookup("conveyor_box");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(*a, "object/cheese_box/shape/0");
  EXPECT_EQ(*b, "object/conveyor_box/shape/0");
}

TEST(CollisionBodyIdMapper, RebuildPopulatesAttachedObjects) {
  RoboPlanCollisionBodyIdMapper mapper;
  std::vector<Snapshot> objs = {
      make_object("cheese_box", /*attached=*/true),
  };
  mapper.rebuild("ur10e", objs, no_robot_mappings());

  auto id = mapper.lookup("cheese_box");
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(*id, "robot/ur10e/attached/cheese_box/shape/0");
}

TEST(CollisionBodyIdMapper, LookupOfUnknownNameIsNullopt) {
  RoboPlanCollisionBodyIdMapper mapper;
  mapper.rebuild("ur10e", {make_object("cheese_box", /*attached=*/false)},
                 no_robot_mappings());
  EXPECT_FALSE(mapper.lookup("wrist_3_link_collision_0").has_value());
  EXPECT_FALSE(mapper.lookup("unknown").has_value());
}

TEST(CollisionBodyIdMapper, RebuildPopulatesRobotCollisionBodies) {
  RoboPlanCollisionBodyIdMapper mapper;
  const auto wrist_body =
      RoboPlanCollisionBodyIdMapper::make_robot_link_id(
          "ur10e", "wrist_3_link", 0);
  const auto shoulder_body =
      RoboPlanCollisionBodyIdMapper::make_robot_link_id(
          "ur10e", "shoulder_link", 1);
  mapper.rebuild(
      "ur10e", {},
      {
          {"wrist_3_link", wrist_body},
          {"wrist_3_link_collision_0", wrist_body},
          {"shoulder_link_collision_1", shoulder_body},
      });

  ASSERT_TRUE(mapper.lookup("wrist_3_link").has_value());
  EXPECT_EQ(*mapper.lookup("wrist_3_link"), wrist_body);
  ASSERT_TRUE(mapper.lookup("wrist_3_link_collision_0").has_value());
  EXPECT_EQ(*mapper.lookup("wrist_3_link_collision_0"), wrist_body);
  ASSERT_TRUE(mapper.lookup("shoulder_link_collision_1").has_value());
  EXPECT_EQ(*mapper.lookup("shoulder_link_collision_1"), shoulder_body);
}

// ---------------------------------------------------------------------------
// Revision semantics
// ---------------------------------------------------------------------------

TEST(CollisionBodyIdMapper, RevisionIncrementsOnEveryRebuild) {
  RoboPlanCollisionBodyIdMapper mapper;
  std::vector<Snapshot> objs = {make_object("a", false)};

  mapper.rebuild("ur10e", objs, no_robot_mappings());
  EXPECT_EQ(mapper.current_revision().revision_id, 1u);

  mapper.rebuild("ur10e", objs, no_robot_mappings());
  EXPECT_EQ(mapper.current_revision().revision_id, 2u);

  mapper.rebuild("ur10e", objs, no_robot_mappings());
  EXPECT_EQ(mapper.current_revision().revision_id, 3u);
}

TEST(CollisionBodyIdMapper, ContentHashIsDeterministicAcrossRebuilds) {
  RoboPlanCollisionBodyIdMapper a;
  RoboPlanCollisionBodyIdMapper b;

  std::vector<Snapshot> objs = {
      make_object("alpha", false),
      make_object("beta", true),
      make_object("gamma", false),
  };
  a.rebuild("ur10e", objs, no_robot_mappings());
  b.rebuild("ur10e", objs, no_robot_mappings());

  EXPECT_EQ(a.current_revision().content_hash, b.current_revision().content_hash);
  EXPECT_FALSE(a.current_revision().content_hash.empty());
}

TEST(CollisionBodyIdMapper, ContentHashIndependentOfInputOrder) {
  RoboPlanCollisionBodyIdMapper a;
  RoboPlanCollisionBodyIdMapper b;

  std::vector<Snapshot> objs_forward = {
      make_object("alpha", false),
      make_object("beta", true),
      make_object("gamma", false),
  };
  std::vector<Snapshot> objs_reverse = {
      make_object("gamma", false),
      make_object("beta", true),
      make_object("alpha", false),
  };
  a.rebuild("ur10e", objs_forward, no_robot_mappings());
  b.rebuild("ur10e", objs_reverse, no_robot_mappings());

  EXPECT_EQ(a.current_revision().content_hash, b.current_revision().content_hash);
}

TEST(CollisionBodyIdMapper, ContentHashChangesWhenAttachStateFlips) {
  RoboPlanCollisionBodyIdMapper mapper;
  mapper.rebuild("ur10e", {make_object("cheese_box", false)},
                 no_robot_mappings());
  const std::string detached_hash = mapper.current_revision().content_hash;

  mapper.rebuild("ur10e", {make_object("cheese_box", true)},
                 no_robot_mappings());
  const std::string attached_hash = mapper.current_revision().content_hash;

  EXPECT_NE(detached_hash, attached_hash);
}

TEST(CollisionBodyIdMapper, ContentHashChangesWhenRobotIdChanges) {
  RoboPlanCollisionBodyIdMapper a;
  RoboPlanCollisionBodyIdMapper b;
  std::vector<Snapshot> objs = {make_object("cheese_box", true)};

  a.rebuild("ur10e", objs, no_robot_mappings());
  b.rebuild("ur5", objs, no_robot_mappings());

  EXPECT_NE(a.current_revision().content_hash, b.current_revision().content_hash);
}

// ---------------------------------------------------------------------------
// Empty rebuild
// ---------------------------------------------------------------------------

TEST(CollisionBodyIdMapper, RebuildWithEmptyObjectsStillBumpsRevision) {
  RoboPlanCollisionBodyIdMapper mapper;
  mapper.rebuild("ur10e", {}, no_robot_mappings());
  EXPECT_EQ(mapper.current_revision().revision_id, 1u);
  EXPECT_FALSE(mapper.current_revision().content_hash.empty());
  EXPECT_FALSE(mapper.lookup("anything").has_value());
}
