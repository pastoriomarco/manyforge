// Chunk 2 unit tests for the RoboPlanProjectionBackend collision-world
// revision publisher and ledger. These tests exercise the integration
// between scene mutations and the publisher against a real RoboPlan
// scene; the trivial publisher arithmetic is covered by the cache
// tests in manyforge_core/test/test_cache.cpp.

#include <manyforge_roboplan/native_kernel_factory.hpp>
#include <manyforge_roboplan/roboplan_focused_adapters.hpp>
#include <manyforge_roboplan/roboplan_projection_backend.hpp>

#include <manyforge_core/collision/collision_policy_evaluator.hpp>
#include <manyforge_core/collision/scene_snapshot_lease.hpp>
#include <manyforge_core/scene_authority.hpp>
#include <manyforge_core/types/reason_codes.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using manyforge::core::AddObjectRequestDTO;
using manyforge::core::AttachObjectRequestDTO;
using manyforge::core::BoxShapeDTO;
using manyforge::core::CollisionGeometryKind;
using manyforge::core::CollisionOwnerKind;
using manyforge::core::CollisionShapeDTO;
using manyforge::core::CollisionWorldBody;
using manyforge::core::CollisionWorldLeaseInventory;
using manyforge::core::CollisionWorldMutationSource;
using manyforge::core::DetachObjectRequestDTO;
using manyforge::core::kErrorInvalidRequest;
using manyforge::core::MeshShapeDTO;
using manyforge::core::Pose3DTO;
using manyforge::core::RemoveObjectRequestDTO;
using manyforge::core::RuntimeCollisionQueryBundle;
using manyforge::core::RuntimeCollisionQueryBundlePtr;
using manyforge::core::RuntimeDynamicSceneCoverage;
using manyforge::core::SceneSnapshotLease;
using manyforge::core::SceneSnapshotLeaseObject;
using manyforge::core::SceneSnapshotLeaseRejection;
using manyforge::core::SceneSnapshotLeaseValidationContext;
using manyforge::core::UpdateObjectPoseRequestDTO;
using manyforge::core::Vector3DTO;
using manyforge::core::ObservedSceneObjectRole;
using manyforge::core::validate_scene_snapshot_lease;
using manyforge::roboplan_backend::create_native_kernel;
using manyforge::roboplan_backend::NativeKernelBundle;
using manyforge::roboplan_backend::RoboPlanProjectionBackend;
using manyforge::roboplan_backend::RoboPlanSceneProjection;

bool has_label(const std::vector<std::string>& labels,
               const std::string& label) {
  return std::find(labels.begin(), labels.end(), label) != labels.end();
}

class CollisionWorldPublisherTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bundle_ = create_native_kernel("ur5_gripper", "arm");
    ASSERT_TRUE(bundle_) << "Failed to create native kernel bundle";
    projection_backend_ = bundle_->projection_backend.get();
    ASSERT_NE(projection_backend_, nullptr);
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

  std::optional<CollisionWorldBody> find_body(const std::string& body_id) {
    auto snapshot = bundle_->scene_authority->snapshot();
    if (!snapshot) {
      return std::nullopt;
    }
    auto it = std::find_if(snapshot->bodies.begin(), snapshot->bodies.end(),
                           [&](const CollisionWorldBody& body) {
                             return body.body_id == body_id;
                           });
    if (it == snapshot->bodies.end()) {
      return std::nullopt;
    }
    return *it;
  }

  static const manyforge::core::SceneSnapshotLiveObject* find_live_object(
      const CollisionWorldLeaseInventory& inventory,
      const std::string& object_id) {
    auto it = std::find_if(inventory.live_objects.begin(),
                           inventory.live_objects.end(),
                           [&](const auto& object) {
                             return object.object_id == object_id;
                           });
    return it == inventory.live_objects.end() ? nullptr : &*it;
  }

  static const CollisionWorldBody* find_body_in_snapshot(
      const manyforge::core::CollisionWorldSnapshot& snapshot,
      const std::string& body_id) {
    auto it = std::find_if(snapshot.bodies.begin(),
                           snapshot.bodies.end(),
                           [&](const CollisionWorldBody& body) {
                             return body.body_id == body_id;
                           });
    return it == snapshot.bodies.end() ? nullptr : &*it;
  }

  static SceneSnapshotLease lease_from_inventory(
      const CollisionWorldLeaseInventory& inventory,
      const std::string& object_id,
      ObservedSceneObjectRole role = ObservedSceneObjectRole::kObservedTarget) {
    SceneSnapshotLease lease;
    lease.lease_id = "lease-" + object_id;
    lease.validation_epoch = 1;
    lease.captured_at_ns = inventory.captured_at_ns;
    lease.lease_max_age_ns = 60'000'000'000LL;
    lease.world_revision = inventory.bundle.world->revision;
    lease.policy_revision = inventory.bundle.policy->revision;
    lease.topology_epoch = inventory.topology_epoch;
    lease.policy_epoch = inventory.policy_epoch;

    const auto* live = find_live_object(inventory, object_id);
    EXPECT_NE(live, nullptr);
    if (live != nullptr) {
      SceneSnapshotLeaseObject object;
      object.object_id = live->object_id;
      object.role = role;
      object.collision_body_ids = live->collision_body_ids;
      object.object_revision_token = live->object_revision_token;
      object.pose = live->pose;
      object.source_timestamp_ns = live->source_timestamp_ns;
      object.source_freshness_budget_ns = live->source_freshness_budget_ns;
      const bool pose_movable_role =
          role == ObservedSceneObjectRole::kSnapshotObstacle ||
          role == ObservedSceneObjectRole::kObservedTarget ||
          role == ObservedSceneObjectRole::kObservedDynamicObstacle;
      if (!pose_movable_role && inventory.bundle.world) {
        for (const auto& body_id : object.collision_body_ids) {
          const auto* body = find_body_in_snapshot(*inventory.bundle.world,
                                                   body_id);
          if (body != nullptr) {
            object.body_content_hashes.emplace(body_id,
                                               body->body_content_hash);
          }
        }
      }
      object.target_drift_tolerance_m = 1.0;
      object.target_angular_tolerance_rad = 1.0;
      object.obstacle_drift_tolerance_m = 1.0;
      object.obstacle_angular_tolerance_rad = 1.0;
      lease.objects.push_back(std::move(object));
    }
    return lease;
  }

  static SceneSnapshotLeaseValidationContext validation_context_from_inventory(
      const CollisionWorldLeaseInventory& inventory,
      std::int64_t now_ns) {
    SceneSnapshotLeaseValidationContext context;
    context.now_ns = now_ns;
    context.live_world_revision = inventory.bundle.world->revision;
    context.live_policy_revision = inventory.bundle.policy->revision;
    context.topology_epoch = inventory.topology_epoch;
    context.policy_epoch = inventory.policy_epoch;
    context.live_objects = inventory.live_objects;
    context.runtime_dynamic_scene_coverage = RuntimeDynamicSceneCoverage{};
    return context;
  }

  manyforge::core::SceneEditResultDTO set_collision_policy(
      manyforge::core::CollisionPolicySnapshot policy) {
    manyforge::core::SetCollisionPolicyRequestDTO request;
    request.policy = std::move(policy);
    return bundle_->scene_authority->set_collision_policy(request);
  }

  std::shared_ptr<NativeKernelBundle> bundle_;
  RoboPlanProjectionBackend* projection_backend_ = nullptr;
  std::string last_mesh_uri_;
};

// --- Initial state -------------------------------------------------------

TEST_F(CollisionWorldPublisherTest, InitialRevisionIsProgramLoadRevision) {
  auto rev = bundle_->scene_authority->published_collision_world_revision();
  EXPECT_EQ(rev.revision_id, 1u);
  EXPECT_FALSE(rev.content_hash.empty());

  auto ledger = projection_backend_->recent_collision_world_ledger();
  ASSERT_EQ(ledger.size(), 1u);
  EXPECT_EQ(ledger[0].mutation_source,
            CollisionWorldMutationSource::kProgramLoad);
  EXPECT_EQ(ledger[0].previous_revision_id, 0u);
}

TEST_F(CollisionWorldPublisherTest, InitialSnapshotPublishesRobotBodies) {
  auto snapshot = bundle_->scene_authority->snapshot();
  ASSERT_NE(snapshot, nullptr);
  const auto current_revision = bundle_->scene_authority->published_collision_world_revision();
  EXPECT_EQ(snapshot->revision.revision_id, current_revision.revision_id);
  EXPECT_EQ(snapshot->revision.content_hash, current_revision.content_hash);
  EXPECT_GT(snapshot->robot_link_count, 0);
  EXPECT_GT(snapshot->geometry_count, 0);
  EXPECT_EQ(snapshot->attached_object_count, 0);
  EXPECT_EQ(snapshot->detached_object_count, 0);
  EXPECT_TRUE(snapshot->all_bodies_have_geometry);
  ASSERT_EQ(snapshot->robot_model_frames.size(), 1u);
  EXPECT_EQ(snapshot->robot_model_frames.front().robot_id, "ur5_gripper");
  EXPECT_EQ(snapshot->robot_model_frames.front().model_frame_id, "base_link");
  EXPECT_DOUBLE_EQ(snapshot->robot_model_frames.front().world_pose.orientation.w,
                   1.0);

  bool found_robot_body = false;
  bool found_robot_mesh = false;
  bool found_robot_proxy = false;
  for (const auto& body : snapshot->bodies) {
    if (body.owner_kind == CollisionOwnerKind::kRobotLink) {
      found_robot_body = true;
      EXPECT_EQ(body.frame_id, "universe");
      ASSERT_FALSE(body.geometries.empty());
      EXPECT_EQ(body.geometries.front().body_id, body.body_id);
      EXPECT_EQ(body.geometries.front().owner_kind, CollisionOwnerKind::kRobotLink);
      if (body.geometries.front().geometry_kind ==
          CollisionGeometryKind::kTriangleMesh) {
        found_robot_mesh = true;
      }
      if (body.geometries.front().geometry_kind == CollisionGeometryKind::kBox &&
          has_label(body.geometries.front().quality_labels,
                    "conservative_proxy")) {
        EXPECT_TRUE(has_label(body.geometries.front().quality_labels,
                              "warning_conservative_aabb_proxy"));
        found_robot_proxy = true;
      }
    }
  }
  EXPECT_TRUE(found_robot_body);
  EXPECT_TRUE(found_robot_mesh || found_robot_proxy);
}

TEST_F(CollisionWorldPublisherTest,
       FocusedProjectionRebuildRestoresExactCommittedScene) {
  ASSERT_TRUE(bundle_->scene_authority
                  ->add_object(make_box_at_origin("rebuild_box"))
                  .success);
  const auto committed_revision = bundle_->scene_authority->current_revision();
  const auto committed_snapshot = bundle_->scene_authority->logical_snapshot();
  ASSERT_NE(committed_snapshot, nullptr);
  ASSERT_EQ(committed_snapshot->objects.size(), 1U);

  // Corrupt only the adapter projection to emulate an uncertain native write;
  // the committed logical snapshot must remain authoritative.
  ASSERT_TRUE(projection_backend_
                  ->remove_object(RemoveObjectRequestDTO{"rebuild_box"})
                  .success);
  EXPECT_NE(projection_backend_->published_collision_world_revision().revision_id,
            committed_revision.revision_id);
  EXPECT_TRUE(projection_backend_->get_scene_objects().empty());

  const auto rebuilt = bundle_->scene_authority->rebuild_projection();

  ASSERT_TRUE(rebuilt.ok())
      << (rebuilt.ok() ? "" : rebuilt.error().message);
  EXPECT_EQ(rebuilt.value().revision_id, committed_revision.revision_id);
  EXPECT_EQ(rebuilt.value().content_hash, committed_revision.content_hash);
  const auto rebuilt_objects = projection_backend_->get_scene_objects();
  ASSERT_EQ(rebuilt_objects.size(), 1U);
  EXPECT_EQ(rebuilt_objects.front().object_id, "rebuild_box");
  ASSERT_NE(bundle_->scene_authority->snapshot(), nullptr);
  EXPECT_EQ(bundle_->scene_authority->snapshot()->revision.content_hash,
            committed_revision.content_hash);
  ASSERT_NE(bundle_->scene_authority->published_runtime_collision_query_bundle(), nullptr);
  EXPECT_EQ(bundle_->scene_authority->published_runtime_collision_query_bundle()
                ->world->revision.content_hash,
            committed_revision.content_hash);
  const auto ledger = projection_backend_->recent_collision_world_ledger(1);
  ASSERT_EQ(ledger.size(), 1U);
  EXPECT_EQ(ledger.front().mutation_source,
            CollisionWorldMutationSource::kProjectionRebuild);
}

// --- Successful mutations bump the revision ------------------------------

TEST_F(CollisionWorldPublisherTest, AddSucceeds_BumpsRevisionOnce) {
  const auto before = bundle_->scene_authority->published_collision_world_revision();

  auto result = bundle_->scene_authority->add_object(make_box_at_origin("box_a"));
  ASSERT_TRUE(result.success) << "add_object should succeed";

  const auto after = bundle_->scene_authority->published_collision_world_revision();
  EXPECT_EQ(after.revision_id, before.revision_id + 1);
  EXPECT_FALSE(after.content_hash.empty());
  EXPECT_NE(after.content_hash, before.content_hash);
}

TEST_F(CollisionWorldPublisherTest,
       SnapshotIncludesDetachedPrimitiveObjectWithDimensions) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);

  const auto body = find_body("object/box_a/shape/0");
  ASSERT_TRUE(body.has_value());
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
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);

  AttachObjectRequestDTO attach;
  attach.object_id = "box_a";
  attach.frame_id = "tool0";
  ASSERT_TRUE(bundle_->scene_authority->attach_object(attach).success);

  const auto detached = find_body("object/box_a/shape/0");
  EXPECT_FALSE(detached.has_value());
  const auto attached =
      find_body("robot/ur5_gripper/attached/box_a/shape/0");
  ASSERT_TRUE(attached.has_value());
  EXPECT_EQ(attached->owner_kind, CollisionOwnerKind::kAttachedObject);
  ASSERT_TRUE(attached->parent_link.has_value());
  EXPECT_EQ(*attached->parent_link, "robot/ur5_gripper/link/tool0");
  EXPECT_EQ(attached->frame_id, "robot/ur5_gripper/link/tool0");
  ASSERT_EQ(attached->geometries.size(), 1u);
  EXPECT_EQ(attached->geometries.front().owner_kind,
            CollisionOwnerKind::kAttachedObject);
}

TEST_F(CollisionWorldPublisherTest,
       SnapshotCarriesMeshSourceIdentityAndMeshFlag) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_mesh_at_origin("mesh_a")).success);

  auto snapshot = bundle_->scene_authority->snapshot();
  ASSERT_NE(snapshot, nullptr);
  EXPECT_TRUE(snapshot->contains_mesh_geometry);
  const auto body = find_body("object/mesh_a/shape/0");
  ASSERT_TRUE(body.has_value());
  ASSERT_EQ(body->geometries.size(), 1u);
  const auto& geom = body->geometries.front();
  EXPECT_EQ(geom.geometry_kind, CollisionGeometryKind::kTriangleMesh);
  ASSERT_TRUE(geom.source_uri.has_value());
  EXPECT_EQ(*geom.source_uri, last_mesh_uri_);
  ASSERT_TRUE(geom.source_hash.has_value());
  EXPECT_FALSE(geom.source_hash->empty());
  ASSERT_TRUE(geom.mesh_scale.has_value());
  EXPECT_DOUBLE_EQ(geom.mesh_scale->x_m, 1.0);
  EXPECT_DOUBLE_EQ(geom.mesh_scale->y_m, 1.0);
  EXPECT_DOUBLE_EQ(geom.mesh_scale->z_m, 1.0);
}

TEST_F(CollisionWorldPublisherTest, RemoveSucceeds_BumpsRevisionOnce) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  const auto before = bundle_->scene_authority->published_collision_world_revision();

  RemoveObjectRequestDTO req;
  req.object_id = "box_a";
  auto result = bundle_->scene_authority->remove_object(req);
  ASSERT_TRUE(result.success);

  EXPECT_EQ(bundle_->scene_authority->published_collision_world_revision().revision_id,
            before.revision_id + 1);
}

TEST_F(CollisionWorldPublisherTest, UpdatePoseSucceeds_BumpsRevisionOnce) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  const auto before = bundle_->scene_authority->published_collision_world_revision();

  UpdateObjectPoseRequestDTO req;
  req.object_id = "box_a";
  req.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  req.pose.orientation.w = 1.0;
  req.frame_id = "universe";
  auto result = bundle_->scene_authority->update_object_pose(req);
  ASSERT_TRUE(result.success);

  EXPECT_EQ(bundle_->scene_authority->published_collision_world_revision().revision_id,
            before.revision_id + 1);
}

TEST_F(CollisionWorldPublisherTest,
       UpdatePoseWithSourceFreshnessReachesCollisionSnapshot) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);

  UpdateObjectPoseRequestDTO req;
  req.object_id = "box_a";
  req.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  req.pose.orientation.w = 1.0;
  req.frame_id = "universe";
  req.source_timestamp_ns = 123'000'000;
  req.source_freshness_budget_ns = 50'000'000;
  auto result = bundle_->scene_authority->update_object_pose(req);
  ASSERT_TRUE(result.success);

  const auto body = find_body("object/box_a/shape/0");
  ASSERT_TRUE(body.has_value());
  ASSERT_TRUE(body->source_timestamp_ns.has_value());
  EXPECT_EQ(*body->source_timestamp_ns, 123'000'000);
  ASSERT_TRUE(body->source_freshness_budget_ns.has_value());
  EXPECT_EQ(*body->source_freshness_budget_ns, 50'000'000);
}

TEST_F(CollisionWorldPublisherTest,
       IdenticalUpdatePoseIsNoOpWithoutRevisionBump) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  const auto before = bundle_->scene_authority->published_collision_world_revision();

  UpdateObjectPoseRequestDTO req;
  req.object_id = "box_a";
  req.pose.position_m = Vector3DTO{0.0, 0.0, 0.5};
  req.pose.orientation.w = 1.0;
  req.frame_id = "universe";
  auto result = bundle_->scene_authority->update_object_pose(req);
  ASSERT_TRUE(result.success);
  ASSERT_TRUE(result.update_status.has_value());
  EXPECT_EQ(*result.update_status, "no_op");
  EXPECT_FALSE(result.collision_world_revision_changed);
  EXPECT_FALSE(result.collision_world_freshness_changed);

  const auto after = bundle_->scene_authority->published_collision_world_revision();
  EXPECT_EQ(after.revision_id, before.revision_id);
  EXPECT_EQ(after.content_hash, before.content_hash);
  const auto counters = projection_backend_->scene_update_status_counters();
  EXPECT_EQ(counters.at("no_op"), 1u);
}

TEST_F(CollisionWorldPublisherTest,
       FreshnessOnlyUpdateRefreshesRuntimeSidecarWithoutRevisionBump) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);

  UpdateObjectPoseRequestDTO first;
  first.object_id = "box_a";
  first.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  first.pose.orientation.w = 1.0;
  first.frame_id = "universe";
  first.source_id = "vision";
  first.source_timestamp_ns = 123'000'000;
  first.source_freshness_budget_ns = 50'000'000;
  auto first_result = bundle_->scene_authority->update_object_pose(first);
  ASSERT_TRUE(first_result.success);
  ASSERT_TRUE(first_result.update_status.has_value());
  EXPECT_EQ(*first_result.update_status, "geometry_changed");

  const auto before = bundle_->scene_authority->published_collision_world_revision();
  auto before_bundle = bundle_->scene_authority->published_runtime_collision_query_bundle();
  ASSERT_TRUE(before_bundle);
  ASSERT_TRUE(before_bundle->world);
  EXPECT_TRUE(before_bundle->lookup_order_valid);
  ASSERT_EQ(before_bundle->body_freshness.size(), 1u);
  EXPECT_EQ(before_bundle->body_freshness.front().body_id,
            "object/box_a/shape/0");
  ASSERT_TRUE(before_bundle->body_freshness.front().source_timestamp_ns);
  EXPECT_EQ(*before_bundle->body_freshness.front().source_timestamp_ns,
            123'000'000);

  UpdateObjectPoseRequestDTO heartbeat = first;
  heartbeat.source_timestamp_ns = 124'000'000;
  auto heartbeat_result = bundle_->scene_authority->update_object_pose(heartbeat);
  ASSERT_TRUE(heartbeat_result.success);
  ASSERT_TRUE(heartbeat_result.update_status.has_value());
  EXPECT_EQ(*heartbeat_result.update_status, "freshness_only");
  EXPECT_FALSE(heartbeat_result.collision_world_revision_changed);
  EXPECT_TRUE(heartbeat_result.collision_world_freshness_changed);

  const auto after = bundle_->scene_authority->published_collision_world_revision();
  EXPECT_EQ(after.revision_id, before.revision_id);
  EXPECT_EQ(after.content_hash, before.content_hash);
  auto after_bundle = bundle_->scene_authority->published_runtime_collision_query_bundle();
  ASSERT_TRUE(after_bundle);
  ASSERT_TRUE(after_bundle->world);
  EXPECT_TRUE(after_bundle->lookup_order_valid);
  EXPECT_EQ(after_bundle->world->revision.revision_id,
            before_bundle->world->revision.revision_id);
  EXPECT_EQ(after_bundle->world, before_bundle->world);
  EXPECT_EQ(after_bundle->policy, before_bundle->policy);
  EXPECT_EQ(after_bundle->policy_index, before_bundle->policy_index);
  EXPECT_GT(after_bundle->epoch, before_bundle->epoch);
  ASSERT_EQ(after_bundle->body_freshness.size(), 1u);
  ASSERT_TRUE(after_bundle->body_freshness.front().source_timestamp_ns);
  EXPECT_EQ(*after_bundle->body_freshness.front().source_timestamp_ns,
            124'000'000);

  const auto body = find_body("object/box_a/shape/0");
  ASSERT_TRUE(body.has_value());
  ASSERT_TRUE(body->source_timestamp_ns.has_value());
  EXPECT_EQ(*body->source_timestamp_ns, 123'000'000)
      << "the immutable world snapshot is not rebuilt for freshness-only";

  std::weak_ptr<const RuntimeCollisionQueryBundle> retired_bundle =
      before_bundle;
  before_bundle.reset();
  EXPECT_FALSE(retired_bundle.expired())
      << "the reader must not become the final owner of a replaced bundle";
  UpdateObjectPoseRequestDTO second_heartbeat = heartbeat;
  second_heartbeat.source_timestamp_ns = 125'000'000;
  ASSERT_TRUE(bundle_->scene_authority->update_object_pose(second_heartbeat).success);
  EXPECT_TRUE(retired_bundle.expired())
      << "retired bundle reclamation belongs to the NRT publication writer";

  const auto counters = projection_backend_->scene_update_status_counters();
  EXPECT_EQ(counters.at("geometry_changed"), 1u);
  EXPECT_EQ(counters.at("freshness_only"), 2u);
  EXPECT_GE(counters.at("runtime_bundle_published"), 4u);
  EXPECT_EQ(counters.at("runtime_bundle_publish_backpressure"), 0u);
}

TEST_F(CollisionWorldPublisherTest,
       FreshnessBudgetChangeRemainsRevisionVisible) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);

  UpdateObjectPoseRequestDTO first;
  first.object_id = "box_a";
  first.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  first.pose.orientation.w = 1.0;
  first.frame_id = "universe";
  first.source_id = "vision";
  first.source_timestamp_ns = 123'000'000;
  first.source_freshness_budget_ns = 50'000'000;
  ASSERT_TRUE(bundle_->scene_authority->update_object_pose(first).success);
  const auto before = bundle_->scene_authority->published_collision_world_revision();

  UpdateObjectPoseRequestDTO changed_budget = first;
  changed_budget.source_timestamp_ns = 124'000'000;
  changed_budget.source_freshness_budget_ns = 60'000'000;
  auto result = bundle_->scene_authority->update_object_pose(changed_budget);
  ASSERT_TRUE(result.success);
  ASSERT_TRUE(result.update_status.has_value());
  EXPECT_EQ(*result.update_status, "geometry_changed");
  EXPECT_TRUE(result.collision_world_revision_changed);

  const auto after = bundle_->scene_authority->published_collision_world_revision();
  EXPECT_EQ(after.revision_id, before.revision_id + 1);
  const auto body = find_body("object/box_a/shape/0");
  ASSERT_TRUE(body.has_value());
  ASSERT_TRUE(body->source_freshness_budget_ns.has_value());
  EXPECT_EQ(*body->source_freshness_budget_ns, 60'000'000);
}

TEST_F(CollisionWorldPublisherTest,
       RuntimeBundleRetirementPressureFailsClosedAndRecovers) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  UpdateObjectPoseRequestDTO update;
  update.object_id = "box_a";
  update.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  update.pose.orientation.w = 1.0;
  update.frame_id = "universe";
  update.source_id = "vision";
  update.source_timestamp_ns = 1;
  update.source_freshness_budget_ns = 1'000'000'000;
  ASSERT_TRUE(bundle_->scene_authority->update_object_pose(update).success);

  std::vector<RuntimeCollisionQueryBundlePtr> held;
  held.reserve(65);
  for (std::size_t i = 0; i < 64; ++i) {
    auto current = bundle_->scene_authority->published_runtime_collision_query_bundle();
    ASSERT_TRUE(current);
    held.push_back(std::move(current));
    update.source_timestamp_ns = static_cast<std::int64_t>(i + 2);
    ASSERT_TRUE(bundle_->scene_authority->update_object_pose(update).success);
  }
  held.push_back(bundle_->scene_authority->published_runtime_collision_query_bundle());
  ASSERT_TRUE(held.back());

  update.source_timestamp_ns = 100;
  const auto rejected = bundle_->scene_authority->update_object_pose(update);
  ASSERT_FALSE(rejected.success);
  ASSERT_TRUE(rejected.error.has_value());
  EXPECT_EQ(rejected.error->details.at("reason"),
            "scene_projection_runtime_publication_mismatch");
  EXPECT_FALSE(bundle_->scene_authority->published_runtime_collision_query_bundle());
  EXPECT_FALSE(bundle_->scene_authority->snapshot());
  EXPECT_FALSE(bundle_->scene_authority->collision_policy_snapshot().has_value());
  EXPECT_EQ(projection_backend_->scene_update_status_counters().at(
                "runtime_bundle_publish_backpressure"),
            1u);

  held.clear();
  const auto rebuilt = bundle_->scene_authority->rebuild_projection();
  ASSERT_TRUE(rebuilt.ok()) << rebuilt.error().message;
  update.source_timestamp_ns = 101;
  ASSERT_TRUE(bundle_->scene_authority->update_object_pose(update).success);
  const auto recovered =
      bundle_->scene_authority->published_runtime_collision_query_bundle();
  ASSERT_TRUE(recovered);
  EXPECT_TRUE(recovered->world);
  EXPECT_TRUE(recovered->policy);
  EXPECT_TRUE(recovered->policy_index);
  EXPECT_TRUE(recovered->lookup_order_valid);
  ASSERT_EQ(recovered->body_freshness.size(), 1u);
  ASSERT_TRUE(recovered->body_freshness.front().source_timestamp_ns);
  EXPECT_EQ(*recovered->body_freshness.front().source_timestamp_ns, 101);
}

TEST_F(CollisionWorldPublisherTest,
       ConcurrentRuntimeBundlePublicationNeverReturnsTornOrTransientNull) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  UpdateObjectPoseRequestDTO update;
  update.object_id = "box_a";
  update.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  update.pose.orientation.w = 1.0;
  update.frame_id = "universe";
  update.source_id = "vision";
  update.source_timestamp_ns = 1;
  update.source_freshness_budget_ns = 1'000'000'000;
  ASSERT_TRUE(bundle_->scene_authority->update_object_pose(update).success);

  std::atomic<bool> done{false};
  std::atomic<bool> invalid_bundle{false};
  std::atomic<std::uint64_t> null_reads{0};
  std::atomic<std::uint64_t> successful_reads{0};
  std::jthread reader([&]() {
    std::uint64_t previous_epoch = 0;
    while (!done.load(std::memory_order_acquire)) {
      auto bundle = bundle_->scene_authority->published_runtime_collision_query_bundle();
      if (!bundle) {
        null_reads.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (!bundle->world || !bundle->policy || !bundle->policy_index ||
          bundle->policy_index->source_policy_revision.revision_id !=
              bundle->policy->revision.revision_id ||
          bundle->policy_index->source_policy_revision.content_hash !=
              bundle->policy->revision.content_hash ||
          !bundle->lookup_order_valid || bundle->epoch < previous_epoch ||
          bundle->body_freshness.size() != 1 ||
          !bundle->body_freshness.front().source_timestamp_ns.has_value()) {
        invalid_bundle.store(true, std::memory_order_release);
        break;
      }
      previous_epoch = bundle->epoch;
      successful_reads.fetch_add(1, std::memory_order_relaxed);
    }
  });
  for (int attempt = 0;
       attempt < 10'000 &&
       successful_reads.load(std::memory_order_acquire) == 0 &&
       !invalid_bundle.load(std::memory_order_acquire);
       ++attempt) {
    std::this_thread::yield();
  }

  bool writer_succeeded = true;
  constexpr std::int64_t kPublicationCount = 1000;
  for (std::int64_t timestamp = 2; timestamp <= kPublicationCount;
       ++timestamp) {
    update.source_timestamp_ns = timestamp;
    if (!bundle_->scene_authority->update_object_pose(update).success) {
      writer_succeeded = false;
      break;
    }
  }
  done.store(true, std::memory_order_release);
  reader.join();

  EXPECT_TRUE(writer_succeeded);
  EXPECT_FALSE(invalid_bundle.load(std::memory_order_acquire));
  EXPECT_GT(successful_reads.load(std::memory_order_relaxed), 0u);
  EXPECT_EQ(null_reads.load(std::memory_order_relaxed), 0u)
      << "ordinary atomic publication must not look like backpressure";
  EXPECT_EQ(projection_backend_->scene_update_status_counters().at(
                "runtime_bundle_publish_backpressure"),
            0u);
  const auto final_bundle =
      bundle_->scene_authority->published_runtime_collision_query_bundle();
  ASSERT_TRUE(final_bundle);
  ASSERT_EQ(final_bundle->body_freshness.size(), 1u);
  ASSERT_TRUE(
      final_bundle->body_freshness.front().source_timestamp_ns.has_value());
  EXPECT_EQ(*final_bundle->body_freshness.front().source_timestamp_ns,
            kPublicationCount);
}

TEST_F(CollisionWorldPublisherTest,
       SourceIdChangeRecomputesBodyHashAndBumpsRevision) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);

  UpdateObjectPoseRequestDTO first;
  first.object_id = "box_a";
  first.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  first.pose.orientation.w = 1.0;
  first.frame_id = "universe";
  first.source_id = "vision_a";
  first.source_timestamp_ns = 123'000'000;
  first.source_freshness_budget_ns = 50'000'000;
  ASSERT_TRUE(bundle_->scene_authority->update_object_pose(first).success);
  const auto before = bundle_->scene_authority->published_collision_world_revision();
  const auto before_body = find_body("object/box_a/shape/0");
  ASSERT_TRUE(before_body.has_value());
  ASSERT_FALSE(before_body->body_content_hash.empty());

  UpdateObjectPoseRequestDTO changed_source = first;
  changed_source.source_id = "vision_b";
  changed_source.source_timestamp_ns = 124'000'000;
  auto result = bundle_->scene_authority->update_object_pose(changed_source);
  ASSERT_TRUE(result.success);
  ASSERT_TRUE(result.update_status.has_value());
  EXPECT_EQ(*result.update_status, "geometry_changed");
  EXPECT_TRUE(result.collision_world_revision_changed);

  const auto after = bundle_->scene_authority->published_collision_world_revision();
  EXPECT_EQ(after.revision_id, before.revision_id + 1);
  const auto after_body = find_body("object/box_a/shape/0");
  ASSERT_TRUE(after_body.has_value());
  EXPECT_NE(after_body->body_content_hash, before_body->body_content_hash);
  ASSERT_TRUE(after_body->source_id.has_value());
  EXPECT_EQ(*after_body->source_id, "vision_b");
}

TEST_F(CollisionWorldPublisherTest,
       RuntimePublishedBundleUpdatesOnSceneAndPolicyWrites) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  const auto after_add_revision = bundle_->scene_authority->published_collision_world_revision();
  auto after_add_bundle =
      bundle_->scene_authority->published_runtime_collision_query_bundle();
  ASSERT_TRUE(after_add_bundle);
  ASSERT_TRUE(after_add_bundle->world);
  EXPECT_TRUE(after_add_bundle->lookup_order_valid);
  EXPECT_EQ(after_add_bundle->world->revision.revision_id,
            after_add_revision.revision_id);
  ASSERT_TRUE(after_add_bundle->policy);
  ASSERT_TRUE(after_add_bundle->policy_index);
  EXPECT_EQ(after_add_bundle->policy_index->source_policy_revision.revision_id,
            after_add_bundle->policy->revision.revision_id);
  EXPECT_EQ(after_add_bundle->policy_index->source_policy_revision.content_hash,
            after_add_bundle->policy->revision.content_hash);
  EXPECT_GT(after_add_bundle->policy->revision.revision_id, 0u);
  EXPECT_TRUE(std::any_of(after_add_bundle->world->bodies.begin(),
                          after_add_bundle->world->bodies.end(),
                          [](const CollisionWorldBody& body) {
                            return body.body_id == "object/box_a/shape/0";
                          }));

  UpdateObjectPoseRequestDTO update;
  update.object_id = "box_a";
  update.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  update.pose.orientation.w = 1.0;
  update.frame_id = "universe";
  update.source_timestamp_ns = 123'000'000;
  update.source_freshness_budget_ns = 50'000'000;
  ASSERT_TRUE(bundle_->scene_authority->update_object_pose(update).success);
  const auto after_update_revision = bundle_->scene_authority->published_collision_world_revision();
  auto after_update_bundle =
      bundle_->scene_authority->published_runtime_collision_query_bundle();
  ASSERT_TRUE(after_update_bundle);
  ASSERT_TRUE(after_update_bundle->world);
  ASSERT_TRUE(after_update_bundle->policy);
  EXPECT_EQ(after_update_bundle->world->revision.revision_id,
            after_update_revision.revision_id);
  EXPECT_EQ(after_update_bundle->policy->revision.revision_id,
            after_add_bundle->policy->revision.revision_id);
  auto body_it = std::find_if(after_update_bundle->world->bodies.begin(),
                              after_update_bundle->world->bodies.end(),
                              [](const CollisionWorldBody& body) {
                                return body.body_id == "object/box_a/shape/0";
                              });
  ASSERT_NE(body_it, after_update_bundle->world->bodies.end());
  ASSERT_TRUE(body_it->source_timestamp_ns.has_value());
  EXPECT_EQ(*body_it->source_timestamp_ns, 123'000'000);

  auto policy = bundle_->scene_projection->configured_collision_policy();
  policy.policy_id = "runtime_policy_override";
  ASSERT_TRUE(set_collision_policy(std::move(policy)).success);
  const auto after_policy_revision = bundle_->scene_authority->published_collision_world_revision();
  auto after_policy_bundle =
      bundle_->scene_authority->published_runtime_collision_query_bundle();
  ASSERT_TRUE(after_policy_bundle);
  ASSERT_TRUE(after_policy_bundle->world);
  ASSERT_TRUE(after_policy_bundle->policy);
  EXPECT_EQ(after_policy_bundle->world->revision.revision_id,
            after_policy_revision.revision_id);
  EXPECT_EQ(after_policy_bundle->policy->policy_id, "runtime_policy_override");
  EXPECT_GT(after_policy_bundle->policy->revision.revision_id,
            after_update_bundle->policy->revision.revision_id);
}

TEST_F(CollisionWorldPublisherTest,
       RetainedLeaseInventorySurvivesLiveSceneAdvance) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);

  auto retained =
      bundle_->scene_authority->retain_scene_snapshot_lease_inventory("lease-retained");
  ASSERT_TRUE(retained.has_value());
  ASSERT_TRUE(retained->bundle.world);
  const auto retained_revision = retained->bundle.world->revision;
  const auto* retained_body =
      find_body_in_snapshot(*retained->bundle.world, "object/box_a/shape/0");
  ASSERT_NE(retained_body, nullptr);
  EXPECT_DOUBLE_EQ(retained_body->pose.position_m.x_m, 0.0);

  UpdateObjectPoseRequestDTO update;
  update.object_id = "box_a";
  update.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  update.pose.orientation.w = 1.0;
  update.frame_id = "universe";
  ASSERT_TRUE(bundle_->scene_authority->update_object_pose(update).success);

  auto current = bundle_->scene_authority->current_scene_snapshot_lease_inventory();
  ASSERT_TRUE(current.has_value());
  ASSERT_TRUE(current->bundle.world);
  EXPECT_NE(current->bundle.world->revision.revision_id,
            retained_revision.revision_id);
  EXPECT_EQ(retained->bundle.world->revision.revision_id,
            retained_revision.revision_id);
  const auto* current_body =
      find_body_in_snapshot(*current->bundle.world, "object/box_a/shape/0");
  ASSERT_NE(current_body, nullptr);
  EXPECT_DOUBLE_EQ(current_body->pose.position_m.x_m, 0.5);
  EXPECT_DOUBLE_EQ(retained_body->pose.position_m.x_m, 0.0);
}

TEST_F(CollisionWorldPublisherTest,
       RetainedLeaseReleaseAndCapacityCleanupWork) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);

  auto retained = bundle_->scene_authority->retain_scene_snapshot_lease_inventory("lease-a");
  ASSERT_TRUE(retained.has_value());
  auto stats = bundle_->scene_authority->scene_snapshot_lease_retention_stats();
  EXPECT_EQ(stats.retained_count, 1u);
  ASSERT_EQ(stats.retained_ids_oldest_first.size(), 1u);
  EXPECT_EQ(stats.retained_ids_oldest_first.front(), "lease-a");

  bundle_->scene_authority->release_scene_snapshot_lease_inventory("lease-a");
  stats = bundle_->scene_authority->scene_snapshot_lease_retention_stats();
  EXPECT_EQ(stats.retained_count, 0u);

  for (std::size_t i = 0; i < stats.retained_capacity; ++i) {
    auto next = bundle_->scene_authority->retain_scene_snapshot_lease_inventory(
        "lease-" + std::to_string(i));
    ASSERT_TRUE(next.has_value());
  }
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_FALSE(bundle_->scene_authority
                     ->retain_scene_snapshot_lease_inventory(
                         "overflow-" + std::to_string(i))
                     .has_value());
  }
  stats = bundle_->scene_authority->scene_snapshot_lease_retention_stats();
  EXPECT_EQ(stats.retained_count, stats.retained_capacity);
  ASSERT_FALSE(stats.retained_ids_oldest_first.empty());
  EXPECT_EQ(stats.retained_ids_oldest_first.front(), "lease-0");
}

TEST_F(CollisionWorldPublisherTest,
       GeneratedLeaseRetentionIdsDoNotClobberCallerProvidedIds) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  auto caller = bundle_->scene_authority->retain_scene_snapshot_lease_inventory(
      "scene_lease_1");
  ASSERT_TRUE(caller.has_value());
  auto generated = bundle_->scene_authority->retain_scene_snapshot_lease_inventory();
  ASSERT_TRUE(generated.has_value());

  EXPECT_NE(generated->retention_id, caller->retention_id);
  auto stats = bundle_->scene_authority->scene_snapshot_lease_retention_stats();
  EXPECT_EQ(stats.retained_count, 2u);
  EXPECT_TRUE(std::find(stats.retained_ids_oldest_first.begin(),
                        stats.retained_ids_oldest_first.end(),
                        caller->retention_id) !=
              stats.retained_ids_oldest_first.end());
  EXPECT_TRUE(std::find(stats.retained_ids_oldest_first.begin(),
                        stats.retained_ids_oldest_first.end(),
                        generated->retention_id) !=
              stats.retained_ids_oldest_first.end());
}

TEST_F(CollisionWorldPublisherTest,
       ObjectRevisionTokenIsStableAcrossPoseOnlyUpdates) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  auto before = bundle_->scene_authority->current_scene_snapshot_lease_inventory();
  ASSERT_TRUE(before.has_value());
  const auto* before_object = find_live_object(*before, "box_a");
  ASSERT_NE(before_object, nullptr);
  ASSERT_TRUE(before_object->object_revision_token.has_value());
  const auto before_token = *before_object->object_revision_token;
  const auto before_topology_epoch = before->topology_epoch;
  const auto before_policy_epoch = before->policy_epoch;
  const auto before_policy_revision = before->bundle.policy->revision;

  UpdateObjectPoseRequestDTO update;
  update.object_id = "box_a";
  update.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  update.pose.orientation.w = 1.0;
  update.frame_id = "universe";
  ASSERT_TRUE(bundle_->scene_authority->update_object_pose(update).success);

  auto after = bundle_->scene_authority->current_scene_snapshot_lease_inventory();
  ASSERT_TRUE(after.has_value());
  const auto* after_object = find_live_object(*after, "box_a");
  ASSERT_NE(after_object, nullptr);
  ASSERT_TRUE(after_object->object_revision_token.has_value());
  EXPECT_EQ(*after_object->object_revision_token, before_token);
  EXPECT_EQ(after->topology_epoch, before_topology_epoch);
  EXPECT_EQ(after->policy_epoch, before_policy_epoch);
  EXPECT_EQ(after->bundle.policy->revision.revision_id,
            before_policy_revision.revision_id);
  EXPECT_EQ(after->bundle.policy->revision.content_hash,
            before_policy_revision.content_hash);
  EXPECT_NE(after->bundle.world->revision.revision_id,
            before->bundle.world->revision.revision_id);
}

TEST_F(CollisionWorldPublisherTest,
       ObjectRevisionTokenChangesOnAttachAndPolicyEpochAdvances) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  auto before = bundle_->scene_authority->current_scene_snapshot_lease_inventory();
  ASSERT_TRUE(before.has_value());
  const auto* before_object = find_live_object(*before, "box_a");
  ASSERT_NE(before_object, nullptr);
  ASSERT_TRUE(before_object->object_revision_token.has_value());

  AttachObjectRequestDTO attach;
  attach.object_id = "box_a";
  attach.frame_id = "tool0";
  ASSERT_TRUE(bundle_->scene_authority->attach_object(attach).success);

  auto after = bundle_->scene_authority->current_scene_snapshot_lease_inventory();
  ASSERT_TRUE(after.has_value());
  const auto* after_object = find_live_object(*after, "box_a");
  ASSERT_NE(after_object, nullptr);
  ASSERT_TRUE(after_object->object_revision_token.has_value());
  EXPECT_NE(*after_object->object_revision_token,
            *before_object->object_revision_token);
  EXPECT_GT(after->topology_epoch, before->topology_epoch);
  EXPECT_GT(after->policy_epoch, before->policy_epoch);
}

TEST_F(CollisionWorldPublisherTest,
       PoseOnlyRevisionDivergenceValidatesAgainstRetainedLease) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  auto retained =
      bundle_->scene_authority->retain_scene_snapshot_lease_inventory("lease-validate");
  ASSERT_TRUE(retained.has_value());
  auto lease = lease_from_inventory(*retained, "box_a");

  UpdateObjectPoseRequestDTO update;
  update.object_id = "box_a";
  update.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  update.pose.orientation.w = 1.0;
  update.frame_id = "universe";
  ASSERT_TRUE(bundle_->scene_authority->update_object_pose(update).success);

  auto current = bundle_->scene_authority->current_scene_snapshot_lease_inventory();
  ASSERT_TRUE(current.has_value());
  EXPECT_NE(current->bundle.world->revision.revision_id,
            retained->bundle.world->revision.revision_id);
  EXPECT_EQ(current->bundle.policy->revision.revision_id,
            retained->bundle.policy->revision.revision_id);
  EXPECT_EQ(current->topology_epoch, retained->topology_epoch);
  EXPECT_EQ(current->policy_epoch, retained->policy_epoch);

  auto context = validation_context_from_inventory(
      *current, retained->captured_at_ns + 1'000'000);
  const auto result = validate_scene_snapshot_lease(lease, context);
  EXPECT_TRUE(result.accepted) << result.reason_code;
  EXPECT_EQ(result.details.at("accepted_live_world_revision_divergence"),
            "true");
}

TEST_F(CollisionWorldPublisherTest,
       SnapshotObstaclePoseOnlyRevisionDivergenceValidatesAgainstRetainedLease) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  auto retained =
      bundle_->scene_authority->retain_scene_snapshot_lease_inventory("lease-snapshot-obstacle");
  ASSERT_TRUE(retained.has_value());
  auto lease = lease_from_inventory(
      *retained, "box_a", ObservedSceneObjectRole::kSnapshotObstacle);

  UpdateObjectPoseRequestDTO update;
  update.object_id = "box_a";
  update.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  update.pose.orientation.w = 1.0;
  update.frame_id = "universe";
  ASSERT_TRUE(bundle_->scene_authority->update_object_pose(update).success);

  auto current = bundle_->scene_authority->current_scene_snapshot_lease_inventory();
  ASSERT_TRUE(current.has_value());
  EXPECT_NE(current->bundle.world->revision.revision_id,
            retained->bundle.world->revision.revision_id);
  EXPECT_EQ(current->topology_epoch, retained->topology_epoch);
  EXPECT_EQ(current->policy_epoch, retained->policy_epoch);

  auto validation = validation_context_from_inventory(
      *current, retained->captured_at_ns + 1'000'000);
  const auto result = validate_scene_snapshot_lease(lease, validation);
  EXPECT_TRUE(result.accepted) << result.reason_code;
  EXPECT_EQ(result.details.at("accepted_live_world_revision_divergence"),
            "true");
}

TEST_F(CollisionWorldPublisherTest,
       SnapshotObstacleAddRemoveDivergenceValidatesAgainstRetainedLease) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  auto retained =
      bundle_->scene_authority->retain_scene_snapshot_lease_inventory("lease-snapshot-topology");
  ASSERT_TRUE(retained.has_value());
  auto lease = lease_from_inventory(
      *retained, "box_a", ObservedSceneObjectRole::kSnapshotObstacle);

  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_b")).success);
  RemoveObjectRequestDTO remove;
  remove.object_id = "box_a";
  ASSERT_TRUE(bundle_->scene_authority->remove_object(remove).success);

  auto current = bundle_->scene_authority->current_scene_snapshot_lease_inventory();
  ASSERT_TRUE(current.has_value());
  EXPECT_GT(current->topology_epoch, retained->topology_epoch);
  EXPECT_EQ(current->policy_epoch, retained->policy_epoch);
  EXPECT_NE(current->bundle.world->revision.revision_id,
            retained->bundle.world->revision.revision_id);
  EXPECT_EQ(find_live_object(*current, "box_a"), nullptr);

  auto validation = validation_context_from_inventory(
      *current, retained->captured_at_ns + 1'000'000);
  const auto result = validate_scene_snapshot_lease(lease, validation);
  EXPECT_TRUE(result.accepted) << result.reason_code;
  EXPECT_EQ(result.details.at("accepted_live_world_revision_divergence"),
            "true");
  EXPECT_EQ(result.details.at("accepted_live_policy_revision_divergence"),
            "true");
  EXPECT_EQ(result.details.at("accepted_live_topology_epoch_divergence"),
            "true");
  EXPECT_EQ(result.details.at("accepted_missing_live_snapshot_obstacle"),
            "true");
}

TEST_F(CollisionWorldPublisherTest,
       SnapshotObstacleRejectsSemanticPolicyUpdateAgainstRetainedLease) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  auto retained =
      bundle_->scene_authority->retain_scene_snapshot_lease_inventory("lease-policy-update");
  ASSERT_TRUE(retained.has_value());
  ASSERT_TRUE(retained->bundle.policy.has_value());
  auto lease = lease_from_inventory(
      *retained, "box_a", ObservedSceneObjectRole::kSnapshotObstacle);

  auto policy = bundle_->scene_projection->configured_collision_policy();
  policy.policy_id = "semantic_policy_override";
  ASSERT_TRUE(set_collision_policy(std::move(policy)).success);

  auto current = bundle_->scene_authority->current_scene_snapshot_lease_inventory();
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(current->topology_epoch, retained->topology_epoch);
  EXPECT_GT(current->policy_epoch, retained->policy_epoch);

  auto validation = validation_context_from_inventory(
      *current, retained->captured_at_ns + 1'000'000);
  const auto result = validate_scene_snapshot_lease(lease, validation);
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.rejection,
            SceneSnapshotLeaseRejection::kCollisionPolicyChangedAfterValidation);
  EXPECT_EQ(result.reason_code,
            manyforge::core::reason_codes::
                kCollisionPolicyChangedAfterValidation);
}

TEST_F(CollisionWorldPublisherTest,
       StaticObstaclePoseOnlyRevisionDivergenceRejectsAgainstRetainedLease) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  auto retained =
      bundle_->scene_authority->retain_scene_snapshot_lease_inventory("lease-static-obstacle");
  ASSERT_TRUE(retained.has_value());
  auto lease = lease_from_inventory(
      *retained, "box_a", ObservedSceneObjectRole::kStaticObstacle);

  UpdateObjectPoseRequestDTO update;
  update.object_id = "box_a";
  update.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  update.pose.orientation.w = 1.0;
  update.frame_id = "universe";
  ASSERT_TRUE(bundle_->scene_authority->update_object_pose(update).success);

  auto current = bundle_->scene_authority->current_scene_snapshot_lease_inventory();
  ASSERT_TRUE(current.has_value());
  auto validation = validation_context_from_inventory(
      *current, retained->captured_at_ns + 1'000'000);
  const auto result = validate_scene_snapshot_lease(lease, validation);
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.reason_code,
            manyforge::core::reason_codes::
                kSceneTopologyChangedAfterValidation);
  EXPECT_EQ(result.details.at("body_content_hash_changed"), "true");
}

TEST_F(CollisionWorldPublisherTest,
       UpdatePoseRejectsPartialSourceFreshnessMetadataWithoutRevisionBump) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  const auto before = bundle_->scene_authority->published_collision_world_revision();

  UpdateObjectPoseRequestDTO req;
  req.object_id = "box_a";
  req.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  req.pose.orientation.w = 1.0;
  req.frame_id = "universe";
  req.source_timestamp_ns = 123'000'000;
  auto result = bundle_->scene_authority->update_object_pose(req);
  ASSERT_FALSE(result.success);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, kErrorInvalidRequest);

  EXPECT_EQ(bundle_->scene_authority->published_collision_world_revision().revision_id,
            before.revision_id);
}

TEST_F(CollisionWorldPublisherTest,
       UpdatePoseRejectsPartialSceneAdmissionMetadataWithoutRevisionBump) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  const auto before = bundle_->scene_authority->published_collision_world_revision();

  UpdateObjectPoseRequestDTO req;
  req.object_id = "box_a";
  req.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  req.pose.orientation.w = 1.0;
  req.frame_id = "universe";
  req.scene_admission_lease_max_age_ns = 2'000'000'000;
  auto result = bundle_->scene_authority->update_object_pose(req);
  ASSERT_FALSE(result.success);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, kErrorInvalidRequest);

  EXPECT_EQ(bundle_->scene_authority->published_collision_world_revision().revision_id,
            before.revision_id);
}

TEST_F(CollisionWorldPublisherTest,
       UpdatePoseRejectsBlankSourceIdWithoutRevisionBump) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  const auto before = bundle_->scene_authority->published_collision_world_revision();

  UpdateObjectPoseRequestDTO req;
  req.object_id = "box_a";
  req.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  req.pose.orientation.w = 1.0;
  req.frame_id = "universe";
  req.source_id = "  ";
  auto result = bundle_->scene_authority->update_object_pose(req);
  ASSERT_FALSE(result.success);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, kErrorInvalidRequest);

  EXPECT_EQ(bundle_->scene_authority->published_collision_world_revision().revision_id,
            before.revision_id);
}

TEST_F(CollisionWorldPublisherTest,
       UpdatePoseRejectsRoleInapplicableSceneAdmissionFieldsWithoutRevisionBump) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  const auto before = bundle_->scene_authority->published_collision_world_revision();

  UpdateObjectPoseRequestDTO req;
  req.object_id = "box_a";
  req.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  req.pose.orientation.w = 1.0;
  req.frame_id = "universe";
  req.scene_admission_role = "snapshot_obstacle";
  req.scene_admission_target_drift_tolerance_m = 0.02;
  auto result = bundle_->scene_authority->update_object_pose(req);
  ASSERT_FALSE(result.success);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, kErrorInvalidRequest);
  EXPECT_EQ(result.error->message,
            "scene_admission target drift tolerances are only valid for "
            "observed_target");

  EXPECT_EQ(bundle_->scene_authority->published_collision_world_revision().revision_id,
            before.revision_id);
}

TEST_F(CollisionWorldPublisherTest,
       SourceAdmissionMetadataPublishesIntoLeaseInventory) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);

  UpdateObjectPoseRequestDTO req;
  req.object_id = "box_a";
  req.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  req.pose.orientation.w = 1.0;
  req.frame_id = "universe";
  req.source_id = "foundationpose";
  req.source_timestamp_ns = 10'000'000;
  req.source_freshness_budget_ns = 500'000'000;
  req.scene_admission_role = "observed_target";
  req.scene_admission_lease_max_age_ns = 2'000'000'000;
  req.scene_admission_target_drift_tolerance_m = 0.02;
  req.scene_admission_target_angular_tolerance_rad = 0.174533;
  ASSERT_TRUE(bundle_->scene_authority->update_object_pose(req).success);

  auto inventory = bundle_->scene_authority->current_scene_snapshot_lease_inventory();
  ASSERT_TRUE(inventory.has_value());
  const auto* live = find_live_object(*inventory, "box_a");
  ASSERT_NE(live, nullptr);
  ASSERT_TRUE(live->source_id.has_value());
  EXPECT_EQ(*live->source_id, "foundationpose");
  ASSERT_TRUE(live->declared_role.has_value());
  EXPECT_EQ(*live->declared_role, ObservedSceneObjectRole::kObservedTarget);
  ASSERT_TRUE(live->lease_max_age_ns.has_value());
  EXPECT_EQ(*live->lease_max_age_ns, 2'000'000'000);
  ASSERT_TRUE(live->target_drift_tolerance_m.has_value());
  EXPECT_DOUBLE_EQ(*live->target_drift_tolerance_m, 0.02);
  ASSERT_TRUE(live->target_angular_tolerance_rad.has_value());
  EXPECT_DOUBLE_EQ(*live->target_angular_tolerance_rad, 0.174533);
}

TEST_F(CollisionWorldPublisherTest, AttachDetachEachBumpRevisionOnce) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  const auto after_add = bundle_->scene_authority->published_collision_world_revision();

  AttachObjectRequestDTO attach;
  attach.object_id = "box_a";
  attach.frame_id = "tool0";
  ASSERT_TRUE(bundle_->scene_authority->attach_object(attach).success);
  const auto after_attach = bundle_->scene_authority->published_collision_world_revision();
  EXPECT_EQ(after_attach.revision_id, after_add.revision_id + 1);

  DetachObjectRequestDTO detach;
  detach.object_id = "box_a";
  detach.frame_id = "universe";
  ASSERT_TRUE(bundle_->scene_authority->detach_object(detach).success);
  const auto after_detach = bundle_->scene_authority->published_collision_world_revision();
  EXPECT_EQ(after_detach.revision_id, after_attach.revision_id + 1);
}

TEST_F(CollisionWorldPublisherTest,
       FactoryRobotIdNamespacesAttachedIdsFromInitialRevision) {
  const auto before = bundle_->scene_authority->published_collision_world_revision();

  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  AttachObjectRequestDTO attach;
  attach.object_id = "box_a";
  attach.frame_id = "tool0";
  ASSERT_TRUE(bundle_->scene_authority->attach_object(attach).success);
  EXPECT_EQ(bundle_->scene_authority->published_collision_world_revision()
                .revision_id,
            before.revision_id + 2);

  auto ledger = projection_backend_->recent_collision_world_ledger(1);
  ASSERT_EQ(ledger.size(), 1u);
  EXPECT_EQ(ledger[0].affected_body_ids,
            (std::vector<std::string>{
                "object/box_a/shape/0",
                "robot/ur5_gripper/attached/box_a/shape/0"}));
}

// --- Rejected mutations do NOT bump --------------------------------------

TEST_F(CollisionWorldPublisherTest, RemoveOfUnknownObject_DoesNotBumpRevision) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  const auto before = bundle_->scene_authority->published_collision_world_revision();

  RemoveObjectRequestDTO req;
  req.object_id = "ghost_object_never_added";
  auto result = bundle_->scene_authority->remove_object(req);
  ASSERT_FALSE(result.success);

  EXPECT_EQ(bundle_->scene_authority->published_collision_world_revision().revision_id,
            before.revision_id);
}

TEST_F(CollisionWorldPublisherTest,
       UpdateOfUnknownObject_DoesNotBumpRevision) {
  const auto before = bundle_->scene_authority->published_collision_world_revision();

  UpdateObjectPoseRequestDTO req;
  req.object_id = "never_added";
  req.pose.orientation.w = 1.0;
  req.frame_id = "universe";
  auto result = bundle_->scene_authority->update_object_pose(req);
  ASSERT_FALSE(result.success);

  EXPECT_EQ(bundle_->scene_authority->published_collision_world_revision().revision_id,
            before.revision_id);
}

TEST_F(CollisionWorldPublisherTest, AttachWithEmptyFrame_DoesNotBumpRevision) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  const auto before = bundle_->scene_authority->published_collision_world_revision();

  AttachObjectRequestDTO attach;
  attach.object_id = "box_a";
  attach.frame_id = "";  // invalid
  auto result = bundle_->scene_authority->attach_object(attach);
  ASSERT_FALSE(result.success);

  EXPECT_EQ(bundle_->scene_authority->published_collision_world_revision().revision_id,
            before.revision_id);
}

// --- Content hash semantics ----------------------------------------------

TEST_F(CollisionWorldPublisherTest,
       ConsecutiveIdenticalPoseUpdatesKeepContentHashAndRevision) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);

  UpdateObjectPoseRequestDTO update;
  update.object_id = "box_a";
  update.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  update.pose.orientation.w = 1.0;
  update.frame_id = "universe";

  auto first_result = bundle_->scene_authority->update_object_pose(update);
  ASSERT_TRUE(first_result.success);
  ASSERT_TRUE(first_result.update_status.has_value());
  EXPECT_EQ(*first_result.update_status, "geometry_changed");
  const auto rev1 = bundle_->scene_authority->published_collision_world_revision();

  auto second_result = bundle_->scene_authority->update_object_pose(update);
  ASSERT_TRUE(second_result.success);
  ASSERT_TRUE(second_result.update_status.has_value());
  EXPECT_EQ(*second_result.update_status, "no_op");
  const auto rev2 = bundle_->scene_authority->published_collision_world_revision();

  EXPECT_EQ(rev2.revision_id, rev1.revision_id);
  EXPECT_EQ(rev2.content_hash, rev1.content_hash);
}

TEST_F(CollisionWorldPublisherTest, PoseChangeChangesContentHash) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  const auto before = bundle_->scene_authority->published_collision_world_revision();

  UpdateObjectPoseRequestDTO update;
  update.object_id = "box_a";
  update.pose.position_m = Vector3DTO{0.5, 0.0, 0.5};
  update.pose.orientation.w = 1.0;
  update.frame_id = "universe";
  ASSERT_TRUE(bundle_->scene_authority->update_object_pose(update).success);

  const auto after = bundle_->scene_authority->published_collision_world_revision();
  EXPECT_EQ(after.revision_id, before.revision_id + 1);
  EXPECT_NE(after.content_hash, before.content_hash);
}

// --- Ledger semantics ----------------------------------------------------

TEST_F(CollisionWorldPublisherTest, LedgerRecordsSuccessfulMutationsInOrder) {
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_a")).success);
  ASSERT_TRUE(bundle_->scene_authority->add_object(make_box_at_origin("box_b")).success);

  AttachObjectRequestDTO attach;
  attach.object_id = "box_a";
  attach.frame_id = "tool0";
  ASSERT_TRUE(bundle_->scene_authority->attach_object(attach).success);

  auto ledger = projection_backend_->recent_collision_world_ledger();
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
                "robot/ur5_gripper/attached/box_a/shape/0"}));
  EXPECT_EQ(ledger[1].affected_body_ids,
            std::vector<std::string>{"object/box_b/shape/0"});
  EXPECT_EQ(ledger[2].affected_body_ids,
            std::vector<std::string>{"object/box_a/shape/0"});
}

TEST_F(CollisionWorldPublisherTest, LedgerLimitReturnsMostRecentEntries) {
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(bundle_->scene_authority->add_object(
        make_box_at_origin("box_" + std::to_string(i))).success);
  }

  auto limited = projection_backend_->recent_collision_world_ledger(2);
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
    ASSERT_TRUE(bundle_->scene_authority->add_object(
        make_box_at_origin("box_" + std::to_string(i))).success);
  }
  auto ledger = projection_backend_->recent_collision_world_ledger();
  EXPECT_LE(ledger.size(), 32u);

  // Newest entry corresponds to the last added object.
  EXPECT_EQ(ledger[0].affected_body_ids,
            std::vector<std::string>{
                "object/box_" + std::to_string(kBeyondCapacity - 1) + "/shape/0"});
}

}  // namespace
