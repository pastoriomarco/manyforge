// Chunk 2 unit tests for the RoboPlanPlanningContext collision-world
// revision publisher and ledger. These tests exercise the integration
// between scene mutations and the publisher against a real RoboPlan
// scene; the trivial publisher arithmetic is covered by the cache
// tests in manyforge_core/test/test_cache.cpp.

#include <manyforge_roboplan/native_kernel_factory.hpp>
#include <manyforge_roboplan/roboplan_planning_context.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

using manyforge::core::AddObjectRequestDTO;
using manyforge::core::AttachObjectRequestDTO;
using manyforge::core::BoxShapeDTO;
using manyforge::core::CollisionGeometryKind;
using manyforge::core::CollisionOwnerKind;
using manyforge::core::CollisionShapeDTO;
using manyforge::core::CollisionWorldBody;
using manyforge::core::CollisionWorldMutationSource;
using manyforge::core::DetachObjectRequestDTO;
using manyforge::core::MeshShapeDTO;
using manyforge::core::Pose3DTO;
using manyforge::core::RemoveObjectRequestDTO;
using manyforge::core::UpdateObjectPoseRequestDTO;
using manyforge::core::Vector3DTO;
using manyforge::roboplan_backend::create_native_kernel;
using manyforge::roboplan_backend::NativeKernelBundle;
using manyforge::roboplan_backend::RoboPlanPlanningContext;

bool has_label(const std::vector<std::string>& labels,
               const std::string& label) {
  return std::find(labels.begin(), labels.end(), label) != labels.end();
}

class CollisionWorldPublisherTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bundle_ = create_native_kernel("ur5_gripper", "arm");
    ASSERT_TRUE(bundle_) << "Failed to create native kernel bundle";
    context_ =
        dynamic_cast<RoboPlanPlanningContext*>(bundle_->context.get());
    ASSERT_NE(context_, nullptr);
  }

  AddObjectRequestDTO make_box_at_origin(const std::string& object_id) {
    AddObjectRequestDTO req;
    req.object_id = object_id;
    req.shape = CollisionShapeDTO{BoxShapeDTO{Vector3DTO{0.1, 0.1, 0.1}}};
    req.pose.position_m = Vector3DTO{0.0, 0.0, 0.5};
    req.pose.orientation.w = 1.0;
    req.frame_id = "universe";
    return req;
  }

  AddObjectRequestDTO make_mesh_at_origin(const std::string& object_id) {
    const auto mesh_path =
        std::filesystem::temp_directory_path() /
        ("manyforge_collision_snapshot_" + object_id + ".obj");
    std::ofstream mesh_file(mesh_path);
    mesh_file << "v 0 0 0\n";
    mesh_file << "v 0.01 0 0\n";
    mesh_file << "v 0 0.01 0\n";
    mesh_file << "f 1 2 3\n";
    mesh_file.close();
    last_mesh_uri_ = mesh_path.string();

    AddObjectRequestDTO req;
    req.object_id = object_id;
    MeshShapeDTO mesh;
    mesh.resource_uri = last_mesh_uri_;
    mesh.scale = Vector3DTO{1.0, 1.0, 1.0};
    req.shape = CollisionShapeDTO{mesh};
    req.pose.position_m = Vector3DTO{0.0, 0.0, 0.5};
    req.pose.orientation.w = 1.0;
    req.frame_id = "universe";
    return req;
  }

  const CollisionWorldBody* find_body(const std::string& body_id) {
    auto snapshot = context_->snapshot();
    if (!snapshot) {
      return nullptr;
    }
    auto it = std::find_if(snapshot->bodies.begin(), snapshot->bodies.end(),
                           [&](const CollisionWorldBody& body) {
                             return body.body_id == body_id;
                           });
    if (it == snapshot->bodies.end()) {
      return nullptr;
    }
    return &*it;
  }

  std::shared_ptr<NativeKernelBundle> bundle_;
  RoboPlanPlanningContext* context_ = nullptr;
  std::string last_mesh_uri_;
};

// --- Initial state -------------------------------------------------------

TEST_F(CollisionWorldPublisherTest, InitialRevisionIsProgramLoadRevision) {
  auto rev = context_->current_collision_world_revision();
  EXPECT_EQ(rev.revision_id, 1u);
  EXPECT_FALSE(rev.content_hash.empty());

  auto ledger = context_->recent_collision_world_ledger();
  ASSERT_EQ(ledger.size(), 1u);
  EXPECT_EQ(ledger[0].mutation_source,
            CollisionWorldMutationSource::kProgramLoad);
  EXPECT_EQ(ledger[0].previous_revision_id, 0u);
}

TEST_F(CollisionWorldPublisherTest, InitialSnapshotPublishesRobotBodies) {
  auto snapshot = context_->snapshot();
  ASSERT_NE(snapshot, nullptr);
  const auto current_revision = context_->current_collision_world_revision();
  EXPECT_EQ(snapshot->revision.revision_id, current_revision.revision_id);
  EXPECT_EQ(snapshot->revision.content_hash, current_revision.content_hash);
  EXPECT_GT(snapshot->robot_link_count, 0);
  EXPECT_GT(snapshot->geometry_count, 0);
  EXPECT_EQ(snapshot->attached_object_count, 0);
  EXPECT_EQ(snapshot->detached_object_count, 0);
  EXPECT_TRUE(snapshot->all_bodies_have_geometry);

  bool found_robot_body = false;
  bool found_robot_proxy = false;
  for (const auto& body : snapshot->bodies) {
    if (body.owner_kind == CollisionOwnerKind::kRobotLink) {
      found_robot_body = true;
      EXPECT_EQ(body.frame_id, "universe");
      ASSERT_FALSE(body.geometries.empty());
      EXPECT_EQ(body.geometries.front().body_id, body.body_id);
      EXPECT_EQ(body.geometries.front().owner_kind, CollisionOwnerKind::kRobotLink);
      if (body.geometries.front().geometry_kind == CollisionGeometryKind::kBox &&
          has_label(body.geometries.front().quality_labels,
                    "conservative_proxy")) {
        found_robot_proxy = true;
      }
    }
  }
  EXPECT_TRUE(found_robot_body);
  EXPECT_TRUE(found_robot_proxy);
}

// --- Successful mutations bump the revision ------------------------------

TEST_F(CollisionWorldPublisherTest, AddSucceeds_BumpsRevisionOnce) {
  const auto before = context_->current_collision_world_revision();

  auto result = context_->add_object(make_box_at_origin("box_a"));
  ASSERT_TRUE(result.success) << "add_object should succeed";

  const auto after = context_->current_collision_world_revision();
  EXPECT_EQ(after.revision_id, before.revision_id + 1);
  EXPECT_FALSE(after.content_hash.empty());
  EXPECT_NE(after.content_hash, before.content_hash);
}

TEST_F(CollisionWorldPublisherTest,
       SnapshotIncludesDetachedPrimitiveObjectWithDimensions) {
  ASSERT_TRUE(context_->add_object(make_box_at_origin("box_a")).success);

  const auto* body = find_body("object/box_a/shape/0");
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(body->owner_kind, CollisionOwnerKind::kDetachedObject);
  EXPECT_FALSE(body->parent_link.has_value());
  EXPECT_EQ(body->frame_id, "universe");
  ASSERT_EQ(body->geometries.size(), 1u);
  const auto& geom = body->geometries.front();
  EXPECT_EQ(geom.geometry_kind, CollisionGeometryKind::kBox);
  ASSERT_TRUE(geom.box_dimensions_m.has_value());
  EXPECT_DOUBLE_EQ(geom.box_dimensions_m->x_m, 0.1);
  EXPECT_DOUBLE_EQ(geom.box_dimensions_m->y_m, 0.1);
  EXPECT_DOUBLE_EQ(geom.box_dimensions_m->z_m, 0.1);
}

TEST_F(CollisionWorldPublisherTest,
       SnapshotRepresentsAttachedObjectInParentLinkFrame) {
  ASSERT_TRUE(context_->add_object(make_box_at_origin("box_a")).success);

  AttachObjectRequestDTO attach;
  attach.object_id = "box_a";
  attach.frame_id = "tool0";
  ASSERT_TRUE(context_->attach_object(attach).success);

  const auto* detached = find_body("object/box_a/shape/0");
  EXPECT_EQ(detached, nullptr);
  const auto* attached =
      find_body("robot/robot0/attached/box_a/shape/0");
  ASSERT_NE(attached, nullptr);
  EXPECT_EQ(attached->owner_kind, CollisionOwnerKind::kAttachedObject);
  ASSERT_TRUE(attached->parent_link.has_value());
  EXPECT_EQ(*attached->parent_link, "robot/robot0/link/tool0");
  EXPECT_EQ(attached->frame_id, "robot/robot0/link/tool0");
  ASSERT_EQ(attached->geometries.size(), 1u);
  EXPECT_EQ(attached->geometries.front().owner_kind,
            CollisionOwnerKind::kAttachedObject);
}

TEST_F(CollisionWorldPublisherTest,
       SnapshotCarriesMeshSourceIdentityAndMeshFlag) {
  ASSERT_TRUE(context_->add_object(make_mesh_at_origin("mesh_a")).success);

  auto snapshot = context_->snapshot();
  ASSERT_NE(snapshot, nullptr);
  EXPECT_TRUE(snapshot->contains_mesh_geometry);
  const auto* body = find_body("object/mesh_a/shape/0");
  ASSERT_NE(body, nullptr);
  ASSERT_EQ(body->geometries.size(), 1u);
  const auto& geom = body->geometries.front();
  EXPECT_EQ(geom.geometry_kind, CollisionGeometryKind::kTriangleMesh);
  ASSERT_TRUE(geom.source_uri.has_value());
  EXPECT_EQ(*geom.source_uri, last_mesh_uri_);
  ASSERT_TRUE(geom.source_hash.has_value());
  EXPECT_FALSE(geom.source_hash->empty());
}

TEST_F(CollisionWorldPublisherTest, RemoveSucceeds_BumpsRevisionOnce) {
  ASSERT_TRUE(context_->add_object(make_box_at_origin("box_a")).success);
  const auto before = context_->current_collision_world_revision();

  RemoveObjectRequestDTO req;
  req.object_id = "box_a";
  auto result = context_->remove_object(req);
  ASSERT_TRUE(result.success);

  EXPECT_EQ(context_->current_collision_world_revision().revision_id,
            before.revision_id + 1);
}

TEST_F(CollisionWorldPublisherTest, UpdatePoseSucceeds_BumpsRevisionOnce) {
  ASSERT_TRUE(context_->add_object(make_box_at_origin("box_a")).success);
  const auto before = context_->current_collision_world_revision();

  UpdateObjectPoseRequestDTO req;
  req.object_id = "box_a";
  req.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  req.pose.orientation.w = 1.0;
  req.frame_id = "universe";
  auto result = context_->update_object_pose(req);
  ASSERT_TRUE(result.success);

  EXPECT_EQ(context_->current_collision_world_revision().revision_id,
            before.revision_id + 1);
}

TEST_F(CollisionWorldPublisherTest, AttachDetachEachBumpRevisionOnce) {
  ASSERT_TRUE(context_->add_object(make_box_at_origin("box_a")).success);
  const auto after_add = context_->current_collision_world_revision();

  AttachObjectRequestDTO attach;
  attach.object_id = "box_a";
  attach.frame_id = "tool0";
  ASSERT_TRUE(context_->attach_object(attach).success);
  const auto after_attach = context_->current_collision_world_revision();
  EXPECT_EQ(after_attach.revision_id, after_add.revision_id + 1);

  DetachObjectRequestDTO detach;
  detach.object_id = "box_a";
  detach.frame_id = "universe";
  ASSERT_TRUE(context_->detach_object(detach).success);
  const auto after_detach = context_->current_collision_world_revision();
  EXPECT_EQ(after_detach.revision_id, after_attach.revision_id + 1);
}

TEST_F(CollisionWorldPublisherTest,
       RobotIdChangeBumpsDeploymentRevisionAndNamespacesAttachedIds) {
  const auto before = context_->current_collision_world_revision();
  context_->set_collision_mapping_robot_id("ur5_cell_a");
  const auto after_robot_id = context_->current_collision_world_revision();
  EXPECT_EQ(after_robot_id.revision_id, before.revision_id + 1);

  ASSERT_TRUE(context_->add_object(make_box_at_origin("box_a")).success);
  AttachObjectRequestDTO attach;
  attach.object_id = "box_a";
  attach.frame_id = "tool0";
  ASSERT_TRUE(context_->attach_object(attach).success);

  auto ledger = context_->recent_collision_world_ledger(1);
  ASSERT_EQ(ledger.size(), 1u);
  EXPECT_EQ(ledger[0].affected_body_ids,
            (std::vector<std::string>{
                "object/box_a/shape/0",
                "robot/ur5_cell_a/attached/box_a/shape/0"}));
}

// --- Rejected mutations do NOT bump --------------------------------------

TEST_F(CollisionWorldPublisherTest, RemoveOfUnknownObject_DoesNotBumpRevision) {
  ASSERT_TRUE(context_->add_object(make_box_at_origin("box_a")).success);
  const auto before = context_->current_collision_world_revision();

  RemoveObjectRequestDTO req;
  req.object_id = "ghost_object_never_added";
  auto result = context_->remove_object(req);
  ASSERT_FALSE(result.success);

  EXPECT_EQ(context_->current_collision_world_revision().revision_id,
            before.revision_id);
}

TEST_F(CollisionWorldPublisherTest,
       UpdateOfUnknownObject_DoesNotBumpRevision) {
  const auto before = context_->current_collision_world_revision();

  UpdateObjectPoseRequestDTO req;
  req.object_id = "never_added";
  req.pose.orientation.w = 1.0;
  req.frame_id = "universe";
  auto result = context_->update_object_pose(req);
  ASSERT_FALSE(result.success);

  EXPECT_EQ(context_->current_collision_world_revision().revision_id,
            before.revision_id);
}

TEST_F(CollisionWorldPublisherTest, AttachWithEmptyFrame_DoesNotBumpRevision) {
  ASSERT_TRUE(context_->add_object(make_box_at_origin("box_a")).success);
  const auto before = context_->current_collision_world_revision();

  AttachObjectRequestDTO attach;
  attach.object_id = "box_a";
  attach.frame_id = "";  // invalid
  auto result = context_->attach_object(attach);
  ASSERT_FALSE(result.success);

  EXPECT_EQ(context_->current_collision_world_revision().revision_id,
            before.revision_id);
}

// --- Content hash semantics ----------------------------------------------

TEST_F(CollisionWorldPublisherTest,
       ConsecutiveIdenticalMutationsKeepContentHashButBumpRevision) {
  ASSERT_TRUE(context_->add_object(make_box_at_origin("box_a")).success);

  UpdateObjectPoseRequestDTO update;
  update.object_id = "box_a";
  update.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  update.pose.orientation.w = 1.0;
  update.frame_id = "universe";

  ASSERT_TRUE(context_->update_object_pose(update).success);
  const auto rev1 = context_->current_collision_world_revision();

  ASSERT_TRUE(context_->update_object_pose(update).success);
  const auto rev2 = context_->current_collision_world_revision();

  EXPECT_EQ(rev2.revision_id, rev1.revision_id + 1);
  EXPECT_EQ(rev2.content_hash, rev1.content_hash);
}

TEST_F(CollisionWorldPublisherTest, PoseChangeChangesContentHash) {
  ASSERT_TRUE(context_->add_object(make_box_at_origin("box_a")).success);
  const auto before = context_->current_collision_world_revision();

  UpdateObjectPoseRequestDTO update;
  update.object_id = "box_a";
  update.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  update.pose.orientation.w = 1.0;
  update.frame_id = "universe";
  ASSERT_TRUE(context_->update_object_pose(update).success);

  const auto after = context_->current_collision_world_revision();
  EXPECT_EQ(after.revision_id, before.revision_id + 1);
  EXPECT_NE(after.content_hash, before.content_hash);
}

// --- Ledger semantics ----------------------------------------------------

TEST_F(CollisionWorldPublisherTest, LedgerRecordsSuccessfulMutationsInOrder) {
  ASSERT_TRUE(context_->add_object(make_box_at_origin("box_a")).success);
  ASSERT_TRUE(context_->add_object(make_box_at_origin("box_b")).success);

  AttachObjectRequestDTO attach;
  attach.object_id = "box_a";
  attach.frame_id = "tool0";
  ASSERT_TRUE(context_->attach_object(attach).success);

  auto ledger = context_->recent_collision_world_ledger();
  ASSERT_EQ(ledger.size(), 4u);
  // Newest-first ordering per implementation.
  EXPECT_EQ(ledger[0].mutation_source, CollisionWorldMutationSource::kAttach);
  EXPECT_EQ(ledger[1].mutation_source, CollisionWorldMutationSource::kSceneEdit);
  EXPECT_EQ(ledger[2].mutation_source, CollisionWorldMutationSource::kSceneEdit);
  EXPECT_EQ(ledger[3].mutation_source,
            CollisionWorldMutationSource::kProgramLoad);
  EXPECT_EQ(ledger[0].revision.revision_id, ledger[1].revision.revision_id + 1);
  EXPECT_EQ(ledger[1].revision.revision_id, ledger[2].revision.revision_id + 1);
  EXPECT_EQ(ledger[0].previous_revision_id, ledger[1].revision.revision_id);
  EXPECT_EQ(ledger[2].previous_revision_id, ledger[3].revision.revision_id);
  EXPECT_EQ(ledger[3].previous_revision_id, 0u);

  // Affected body ids use the normalized CollisionBodyId namespace.
  EXPECT_EQ(ledger[0].affected_body_ids,
            (std::vector<std::string>{
                "object/box_a/shape/0",
                "robot/robot0/attached/box_a/shape/0"}));
  EXPECT_EQ(ledger[1].affected_body_ids,
            std::vector<std::string>{"object/box_b/shape/0"});
  EXPECT_EQ(ledger[2].affected_body_ids,
            std::vector<std::string>{"object/box_a/shape/0"});
}

TEST_F(CollisionWorldPublisherTest, LedgerLimitReturnsMostRecentEntries) {
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(context_->add_object(
        make_box_at_origin("box_" + std::to_string(i))).success);
  }

  auto limited = context_->recent_collision_world_ledger(2);
  ASSERT_EQ(limited.size(), 2u);
  EXPECT_EQ(limited[0].affected_body_ids,
            std::vector<std::string>{"object/box_4/shape/0"});
  EXPECT_EQ(limited[1].affected_body_ids,
            std::vector<std::string>{"object/box_3/shape/0"});
}

TEST_F(CollisionWorldPublisherTest, LedgerIsBoundedAtCapacity) {
  // Add more than the kCollisionWorldLedgerCapacity default (32) so the
  // oldest entries are dropped.
  constexpr int kBeyondCapacity = 40;
  for (int i = 0; i < kBeyondCapacity; ++i) {
    ASSERT_TRUE(context_->add_object(
        make_box_at_origin("box_" + std::to_string(i))).success);
  }
  auto ledger = context_->recent_collision_world_ledger();
  EXPECT_LE(ledger.size(), 32u);

  // Newest entry corresponds to the last added object.
  EXPECT_EQ(ledger[0].affected_body_ids,
            std::vector<std::string>{
                "object/box_" + std::to_string(kBeyondCapacity - 1) + "/shape/0"});
}

}  // namespace
