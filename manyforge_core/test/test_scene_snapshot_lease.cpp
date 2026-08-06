#include <manyforge_core/collision/scene_snapshot_lease.hpp>

#include <manyforge_core/types/reason_codes.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

constexpr auto kDigestA =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr auto kDigestB =
    "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr auto kDigestC =
    "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
constexpr auto kDigestD =
    "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";

ProjectionRevisionDTO make_projection_revision(std::uint64_t scene_revision = 7) {
  return ProjectionRevisionDTO{
      RetainedBundleId{kDigestA}, GraphRevision{kDigestB},
      SceneRevision{SceneId{kDigestC}, scene_revision,
                    ContentDigest{kDigestD}}};
}

Pose3DTO pose_at(double x_m, double yaw_hint = 0.0) {
  Pose3DTO pose;
  pose.position_m = Vector3DTO{x_m, 0.0, 0.0};
  pose.orientation = QuaternionDTO{0.0, 0.0, yaw_hint, 1.0};
  return pose;
}

SceneSnapshotLeaseObject make_object(
    std::string id,
    ObservedSceneObjectRole role = ObservedSceneObjectRole::kObservedTarget) {
  SceneSnapshotLeaseObject object;
  object.object_id = std::move(id);
  object.role = role;
  object.collision_body_ids = {"scene/" + object.object_id + "/shape/0"};
  object.object_revision_token = object.object_id + "-shape-v1";
  object.body_content_hashes.emplace(object.collision_body_ids.front(),
                                     object.object_id + "-body-hash-v1");
  object.pose = pose_at(0.0);
  object.source_id = "foundationpose";
  object.source_timestamp_ns = 1'000;
  object.source_freshness_budget_ns = 10'000;
  object.target_drift_tolerance_m = 0.010;
  object.target_angular_tolerance_rad = 0.010;
  object.obstacle_drift_tolerance_m = 0.010;
  object.obstacle_angular_tolerance_rad = 0.010;
  return object;
}

SceneSnapshotLiveObject live_from(const SceneSnapshotLeaseObject& object) {
  SceneSnapshotLiveObject live;
  live.object_id = object.object_id;
  live.collision_body_ids = object.collision_body_ids;
  live.object_revision_token = object.object_revision_token;
  live.body_content_hashes = object.body_content_hashes;
  live.pose = object.pose;
  live.source_id = object.source_id;
  live.source_timestamp_ns = 1'100;
  live.source_freshness_budget_ns = 10'000;
  return live;
}

SceneSnapshotLease make_lease(
    ObservedSceneObjectRole role = ObservedSceneObjectRole::kObservedTarget) {
  SceneSnapshotLease lease;
  lease.lease_id = "lease-1";
  lease.validation_epoch = 42;
  lease.captured_at_ns = 1'000;
  lease.lease_max_age_ns = 10'000;
  lease.projection_revision = make_projection_revision();
  lease.world_revision = CollisionWorldRevision{7, "world-hash-v1"};
  lease.policy_revision = CollisionPolicyRevision{3, "policy-hash-v1"};
  lease.topology_epoch = 11;
  lease.policy_epoch = 5;
  lease.objects.push_back(make_object("box", role));
  return lease;
}

SceneSnapshotLeaseValidationContext make_context(
    const SceneSnapshotLease& lease) {
  SceneSnapshotLeaseValidationContext context;
  context.now_ns = 1'200;
  context.live_projection_revision = lease.projection_revision;
  context.live_world_revision = lease.world_revision;
  context.live_policy_revision = lease.policy_revision;
  context.topology_epoch = lease.topology_epoch;
  context.policy_epoch = lease.policy_epoch;
  for (const auto& object : lease.objects) {
    context.live_objects.push_back(live_from(object));
  }
  return context;
}

void expect_rejected(const SceneSnapshotLeaseValidationResult& result,
                     SceneSnapshotLeaseRejection rejection,
                     const char* reason_code) {
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.rejection, rejection);
  EXPECT_EQ(result.reason_code, reason_code);
}

TEST(SceneSnapshotLeaseValidator, UnchangedLiveSceneAccepts) {
  const auto lease = make_lease();
  const auto result = validate_scene_snapshot_lease(lease, make_context(lease));

  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.reason_code, "");
  EXPECT_EQ(result.lease_id, lease.lease_id);
  ASSERT_TRUE(result.lease_age_ns.has_value());
  EXPECT_EQ(*result.lease_age_ns, 200);
}

TEST(SceneSnapshotLeaseValidator,
     AdvancedLiveRevisionWithUnchangedRelevantContentAccepts) {
  const auto lease = make_lease();
  auto context = make_context(lease);
  context.live_world_revision = CollisionWorldRevision{8, "world-hash-v2"};

  const auto result = validate_scene_snapshot_lease(lease, context);

  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.live_world_revision.revision_id, 8u);
  EXPECT_EQ(result.details.at("accepted_live_world_revision_divergence"),
            "true");
}

TEST(SceneSnapshotLeaseValidator, StructuralProjectionChangeRejects) {
  const auto lease = make_lease();
  auto context = make_context(lease);
  context.live_projection_revision->graph_revision = GraphRevision{kDigestC};

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(
      result,
      SceneSnapshotLeaseRejection::
          kProjectionStructureChangedAfterValidation,
      reason_codes::kProjectionStructureChangedAfterValidation);
}

TEST(SceneSnapshotLeaseValidator, MissingLeaseRejectsWithStableReason) {
  SceneSnapshotLeaseValidationContext context;
  context.live_world_revision = CollisionWorldRevision{8, "world"};
  context.live_policy_revision = CollisionPolicyRevision{2, "policy"};

  const auto result =
      validate_scene_snapshot_lease(std::optional<SceneSnapshotLease>{},
                                    context);

  expect_rejected(result,
                  SceneSnapshotLeaseRejection::kSceneSnapshotLeaseMissing,
                  reason_codes::kSceneSnapshotLeaseMissing);
}

TEST(SceneSnapshotLeaseValidator, LiveSnapshotUnavailableRejects) {
  const auto lease = make_lease();
  auto context = make_context(lease);
  context.live_world_revision.reset();

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(result,
                  SceneSnapshotLeaseRejection::kLiveSceneSnapshotUnavailable,
                  reason_codes::kLiveSceneSnapshotUnavailable);
}

TEST(SceneSnapshotLeaseValidator, LeaseAgeExpiredRejects) {
  auto lease = make_lease();
  lease.lease_max_age_ns = 100;
  auto context = make_context(lease);
  context.now_ns = 1'200;

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(result,
                  SceneSnapshotLeaseRejection::kSceneSnapshotLeaseExpired,
                  reason_codes::kSceneSnapshotLeaseExpired);
  EXPECT_EQ(result.details.at("lease_max_age_ns"), "100");
}

TEST(SceneSnapshotLeaseValidator, StaleObservedSourceRejects) {
  auto lease = make_lease();
  lease.objects.front().source_timestamp_ns = 1'000;
  lease.objects.front().source_freshness_budget_ns = 99;
  auto context = make_context(lease);
  context.now_ns = 1'200;

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(result,
                  SceneSnapshotLeaseRejection::kObservedSceneSourceStale,
                  reason_codes::kObservedSceneSourceStale);
  ASSERT_TRUE(result.object_id.has_value());
  EXPECT_EQ(*result.object_id, "box");
  ASSERT_TRUE(result.source_age_ns.has_value());
  EXPECT_EQ(*result.source_age_ns, 200);
}

TEST(SceneSnapshotLeaseValidator, FutureSourceTimestampRejectsAsClockMismatch) {
  auto lease = make_lease();
  lease.objects.front().source_timestamp_ns = 1'500;
  lease.objects.front().source_freshness_budget_ns.reset();
  auto context = make_context(lease);
  context.now_ns = 1'200;

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(result,
                  SceneSnapshotLeaseRejection::kObservedSceneSourceStale,
                  reason_codes::kObservedSceneSourceStale);
  ASSERT_TRUE(result.source_age_ns.has_value());
  EXPECT_EQ(*result.source_age_ns, -300);
}

TEST(SceneSnapshotLeaseValidator,
     UnrelatedTopologyEpochChangeAcceptsWithDivergenceEvidence) {
  const auto lease = make_lease();
  auto context = make_context(lease);
  ++context.topology_epoch;

  const auto result = validate_scene_snapshot_lease(lease, context);

  EXPECT_TRUE(result.accepted) << result.reason_code;
  EXPECT_EQ(result.details.at("live_topology_epoch_diverged"), "true");
  EXPECT_EQ(result.details.at("accepted_live_topology_epoch_divergence"),
            "true");
}

TEST(SceneSnapshotLeaseValidator,
     CollisionAuthorityObjectWithoutIdentityRejects) {
  auto lease = make_lease();
  lease.objects.front().object_revision_token.reset();
  lease.objects.front().body_content_hashes.clear();
  auto context = make_context(lease);

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(
      result,
      SceneSnapshotLeaseRejection::kSceneTopologyChangedAfterValidation,
      reason_codes::kSceneTopologyChangedAfterValidation);
  EXPECT_EQ(result.details.at("missing_collision_object_identity"), "true");
}

TEST(SceneSnapshotLeaseValidator, DuplicateLeaseObjectIdsReject) {
  auto lease = make_lease();
  lease.objects.push_back(lease.objects.front());
  auto context = make_context(lease);

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(
      result,
      SceneSnapshotLeaseRejection::kSceneTopologyChangedAfterValidation,
      reason_codes::kSceneTopologyChangedAfterValidation);
  EXPECT_EQ(result.details.at("duplicate_lease_object_id"), "true");
}

TEST(SceneSnapshotLeaseValidator, DuplicateLiveObjectIdsReject) {
  const auto lease = make_lease();
  auto context = make_context(lease);
  context.live_objects.push_back(context.live_objects.front());

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(
      result,
      SceneSnapshotLeaseRejection::kSceneTopologyChangedAfterValidation,
      reason_codes::kSceneTopologyChangedAfterValidation);
  EXPECT_EQ(result.details.at("duplicate_live_object_id"), "true");
}

TEST(SceneSnapshotLeaseValidator,
     BodyInventoryChangeRejectsEvenIfEpochWasMiswired) {
  const auto lease = make_lease();
  auto context = make_context(lease);
  context.live_objects.front().collision_body_ids.push_back(
      "scene/box/shape/1");

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(
      result,
      SceneSnapshotLeaseRejection::kSceneTopologyChangedAfterValidation,
      reason_codes::kSceneTopologyChangedAfterValidation);
  EXPECT_EQ(result.details.at("collision_body_ids_changed"), "true");
}

TEST(SceneSnapshotLeaseValidator, ShapeTokenChangeRejects) {
  const auto lease = make_lease();
  auto context = make_context(lease);
  context.live_objects.front().object_revision_token = "box-shape-v2";

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(
      result,
      SceneSnapshotLeaseRejection::kSceneTopologyChangedAfterValidation,
      reason_codes::kSceneTopologyChangedAfterValidation);
  EXPECT_EQ(result.details.at("object_revision_token_changed"), "true");
}

TEST(SceneSnapshotLeaseValidator, BodyHashChangeRejects) {
  const auto lease = make_lease();
  auto context = make_context(lease);
  context.live_objects.front().body_content_hashes.begin()->second =
      "new-body-hash";

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(
      result,
      SceneSnapshotLeaseRejection::kSceneTopologyChangedAfterValidation,
      reason_codes::kSceneTopologyChangedAfterValidation);
  EXPECT_EQ(result.details.at("body_content_hash_changed"), "true");
}

TEST(SceneSnapshotLeaseValidator, PolicyChangeRejects) {
  const auto lease = make_lease();
  auto context = make_context(lease);
  context.live_policy_revision = CollisionPolicyRevision{4, "policy-hash-v2"};
  ++context.policy_epoch;

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(
      result,
      SceneSnapshotLeaseRejection::kCollisionPolicyChangedAfterValidation,
      reason_codes::kCollisionPolicyChangedAfterValidation);
}

TEST(SceneSnapshotLeaseValidator,
     PolicyRevisionDivergenceWithStableEpochAcceptsWithEvidence) {
  const auto lease = make_lease();
  auto context = make_context(lease);
  context.live_policy_revision = CollisionPolicyRevision{4, "policy-hash-v2"};

  const auto result = validate_scene_snapshot_lease(lease, context);

  EXPECT_TRUE(result.accepted) << result.reason_code;
  EXPECT_EQ(result.details.at("accepted_live_policy_revision_divergence"),
            "true");
}

TEST(SceneSnapshotLeaseValidator, TargetDriftWithinToleranceAccepts) {
  const auto lease = make_lease();
  auto context = make_context(lease);
  context.live_objects.front().pose = pose_at(0.005);

  const auto result = validate_scene_snapshot_lease(lease, context);

  EXPECT_TRUE(result.accepted);
  ASSERT_TRUE(result.target_drift_m.has_value());
  EXPECT_NEAR(*result.target_drift_m, 0.005, 1e-12);
}

TEST(SceneSnapshotLeaseValidator, MissingLiveTargetPoseRejectsFailClosed) {
  const auto lease = make_lease();
  auto context = make_context(lease);
  context.live_objects.front().pose.reset();

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(result,
                  SceneSnapshotLeaseRejection::kObservedSceneSourceStale,
                  reason_codes::kObservedSceneSourceStale);
  EXPECT_EQ(result.details.at("live_pose_unavailable"), "true");
}

TEST(SceneSnapshotLeaseValidator,
     TargetDriftBeyondToleranceRejectsWithReplanReason) {
  const auto lease = make_lease();
  auto context = make_context(lease);
  context.live_objects.front().pose = pose_at(0.020);

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(result,
                  SceneSnapshotLeaseRejection::kTargetMovedAfterValidation,
                  reason_codes::kTargetMovedAfterValidation);
  ASSERT_TRUE(result.target_drift_m.has_value());
  EXPECT_NEAR(*result.target_drift_m, 0.020, 1e-12);
}

TEST(SceneSnapshotLeaseValidator,
     SnapshotObstaclePoseDriftAcceptsFrozenValidationSnapshot) {
  auto lease = make_lease(ObservedSceneObjectRole::kSnapshotObstacle);
  lease.objects.front().body_content_hashes.clear();
  auto context = make_context(lease);
  context.live_world_revision = CollisionWorldRevision{8, "world-hash-v2"};
  context.live_objects.front().pose = pose_at(0.250);

  const auto result = validate_scene_snapshot_lease(lease, context);

  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.details.at("accepted_live_world_revision_divergence"),
            "true");
  EXPECT_FALSE(result.obstacle_drift_m.has_value());
  EXPECT_FALSE(result.target_drift_m.has_value());
}

TEST(SceneSnapshotLeaseValidator,
     SnapshotObstacleBodyIdentityChangeAcceptsFrozenValidationSnapshot) {
  auto lease = make_lease(ObservedSceneObjectRole::kSnapshotObstacle);
  lease.objects.front().body_content_hashes.clear();
  auto context = make_context(lease);
  context.live_world_revision = CollisionWorldRevision{8, "world-hash-v2"};
  ++context.topology_epoch;
  context.live_objects.front().collision_body_ids.push_back(
      "scene/box/shape/1");
  context.live_objects.front().object_revision_token = "box-shape-v2";

  const auto result = validate_scene_snapshot_lease(lease, context);

  EXPECT_TRUE(result.accepted) << result.reason_code;
  EXPECT_EQ(result.details.at("accepted_live_world_revision_divergence"),
            "true");
  EXPECT_EQ(result.details.at("accepted_live_topology_epoch_divergence"),
            "true");
  EXPECT_EQ(result.details.at(
                "accepted_snapshot_obstacle_body_inventory_divergence"),
            "true");
  EXPECT_EQ(result.details.at("accepted_snapshot_obstacle_token_divergence"),
            "true");
}

TEST(SceneSnapshotLeaseValidator,
     SnapshotObstacleMissingLiveObjectAcceptsFrozenValidationSnapshot) {
  auto lease = make_lease(ObservedSceneObjectRole::kSnapshotObstacle);
  lease.objects.front().body_content_hashes.clear();
  auto context = make_context(lease);
  context.live_world_revision = CollisionWorldRevision{8, "world-hash-v2"};
  ++context.topology_epoch;
  context.live_objects.clear();

  const auto result = validate_scene_snapshot_lease(lease, context);

  EXPECT_TRUE(result.accepted) << result.reason_code;
  EXPECT_EQ(result.details.at("accepted_missing_live_snapshot_obstacle"),
            "true");
  EXPECT_EQ(result.details.at("accepted_live_topology_epoch_divergence"),
            "true");
}

TEST(SceneSnapshotLeaseValidator,
     SnapshotObstacleRequiresCaptureTimeRevisionToken) {
  auto lease = make_lease(ObservedSceneObjectRole::kSnapshotObstacle);
  lease.objects.front().object_revision_token.reset();
  auto context = make_context(lease);

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(
      result,
      SceneSnapshotLeaseRejection::kSceneTopologyChangedAfterValidation,
      reason_codes::kSceneTopologyChangedAfterValidation);
  EXPECT_EQ(result.details.at("missing_collision_object_identity"), "true");
}

TEST(SceneSnapshotLeaseValidator,
     SnapshotObstacleIgnoresPoseBearingBodyHashWhenTokenMatches) {
  auto lease = make_lease(ObservedSceneObjectRole::kSnapshotObstacle);
  auto context = make_context(lease);
  context.live_world_revision = CollisionWorldRevision{8, "world-hash-v2"};
  context.live_objects.front().pose = pose_at(0.250);
  context.live_objects.front().body_content_hashes[
      lease.objects.front().collision_body_ids.front()] = "pose-bearing-v2";

  const auto result = validate_scene_snapshot_lease(lease, context);

  EXPECT_TRUE(result.accepted) << result.reason_code;
  EXPECT_EQ(result.details.at("accepted_live_world_revision_divergence"),
            "true");
}

TEST(SceneSnapshotLeaseValidator,
     StaticObstacleBodyHashChangeRejectsPoseOrContentMovement) {
  auto lease = make_lease(ObservedSceneObjectRole::kStaticObstacle);
  auto context = make_context(lease);
  context.live_world_revision = CollisionWorldRevision{8, "world-hash-v2"};
  context.live_objects.front().pose = pose_at(0.250);
  context.live_objects.front().body_content_hashes[
      lease.objects.front().collision_body_ids.front()] = "box-body-hash-v2";

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(result,
                  SceneSnapshotLeaseRejection::
                      kSceneTopologyChangedAfterValidation,
                  reason_codes::kSceneTopologyChangedAfterValidation);
  EXPECT_EQ(result.details.at("body_content_hash_changed"), "true");
}

TEST(SceneSnapshotLeaseValidator, ObstacleDriftWithinToleranceAccepts) {
  const auto lease =
      make_lease(ObservedSceneObjectRole::kObservedDynamicObstacle);
  auto context = make_context(lease);
  context.live_objects.front().pose = pose_at(0.005);

  const auto result = validate_scene_snapshot_lease(lease, context);

  EXPECT_TRUE(result.accepted);
  ASSERT_TRUE(result.obstacle_drift_m.has_value());
  EXPECT_NEAR(*result.obstacle_drift_m, 0.005, 1e-12);
}

TEST(SceneSnapshotLeaseValidator,
     ObstacleDriftBeyondToleranceRejectsWithoutRuntimeDynamicPolicy) {
  const auto lease =
      make_lease(ObservedSceneObjectRole::kObservedDynamicObstacle);
  auto context = make_context(lease);
  context.live_objects.front().pose = pose_at(0.020);

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(result,
                  SceneSnapshotLeaseRejection::kObstacleMovedAfterValidation,
                  reason_codes::kObstacleMovedAfterValidation);
  ASSERT_TRUE(result.obstacle_drift_m.has_value());
  EXPECT_NEAR(*result.obstacle_drift_m, 0.020, 1e-12);
}

TEST(SceneSnapshotLeaseValidator,
     DynamicObstacleDriftRejectsWhenRuntimeCoverageIsNotArmed) {
  auto lease = make_lease(ObservedSceneObjectRole::kObservedDynamicObstacle);
  lease.objects.front().runtime_dynamic_obstacle_allowed = true;
  auto context = make_context(lease);
  context.live_objects.front().pose = pose_at(0.020);
  context.runtime_dynamic_scene_coverage.configured = true;
  context.runtime_dynamic_scene_coverage.armed = false;
  context.runtime_dynamic_scene_coverage.fresh = true;
  context.runtime_dynamic_scene_coverage.qualified = true;

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(
      result,
      SceneSnapshotLeaseRejection::kRuntimeDynamicSceneNotQualified,
      reason_codes::kRuntimeDynamicSceneNotQualified);
  EXPECT_EQ(result.details.at("runtime_dynamic_coverage_armed"), "false");
}

TEST(SceneSnapshotLeaseValidator,
     DynamicObstacleDriftAcceptsWithQualifiedRuntimeCoverage) {
  auto lease = make_lease(ObservedSceneObjectRole::kObservedDynamicObstacle);
  lease.objects.front().runtime_dynamic_obstacle_allowed = true;
  auto context = make_context(lease);
  context.live_objects.front().pose = pose_at(0.020);
  context.runtime_dynamic_scene_coverage.configured = true;
  context.runtime_dynamic_scene_coverage.armed = true;
  context.runtime_dynamic_scene_coverage.fresh = true;
  context.runtime_dynamic_scene_coverage.qualified = true;
  context.runtime_dynamic_scene_coverage.coverage_id = "runtime-monitor";

  const auto result = validate_scene_snapshot_lease(lease, context);

  EXPECT_TRUE(result.accepted);
  ASSERT_TRUE(result.obstacle_drift_m.has_value());
  EXPECT_EQ(result.details.at("accepted_runtime_dynamic_obstacle_drift"),
            "true");
  EXPECT_EQ(result.details.at("runtime_dynamic_coverage_id"),
            "runtime-monitor");
}

}  // namespace
}  // namespace manyforge::core
