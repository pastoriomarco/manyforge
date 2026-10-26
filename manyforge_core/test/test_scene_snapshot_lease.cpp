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

SceneSnapshotLeaseObject make_object(std::string id) {
  SceneSnapshotLeaseObject object;
  object.object_id = std::move(id);
  object.collision_body_ids = {"scene/" + object.object_id + "/shape/0"};
  object.object_revision_token = object.object_id + "-shape-v1";
  object.body_content_hashes.emplace(object.collision_body_ids.front(),
                                     object.object_id + "-body-hash-v1");
  object.pose = pose_at(0.0);
  return object;
}

SceneSnapshotLiveObject live_from(const SceneSnapshotLeaseObject& object) {
  SceneSnapshotLiveObject live;
  live.object_id = object.object_id;
  live.collision_body_ids = object.collision_body_ids;
  live.object_revision_token = object.object_revision_token;
  live.body_content_hashes = object.body_content_hashes;
  live.pose = object.pose;
  live.source_id = "foundationpose";
  live.source_timestamp_ns = 1'100;
  live.source_freshness_budget_ns = 10'000;
  return live;
}

SceneSnapshotLease make_lease() {
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
  lease.objects.push_back(make_object("box"));
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

bool has_object_divergence_detail(
    const SceneSnapshotLeaseValidationResult& result) {
  for (const auto& [key, value] : result.details) {
    (void)value;
    if (key.starts_with("object_divergence")) {
      return true;
    }
  }
  return false;
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

TEST(SceneSnapshotLeaseValidator, StaleLiveSourceRejects) {
  const auto lease = make_lease();
  auto context = make_context(lease);
  context.live_objects.front().source_timestamp_ns = 1'000;
  context.live_objects.front().source_freshness_budget_ns = 99;
  context.now_ns = 1'200;

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(result,
                  SceneSnapshotLeaseRejection::kObservedSceneSourceStale,
                  reason_codes::kObservedSceneSourceStale);
  ASSERT_TRUE(result.object_id.has_value());
  EXPECT_EQ(*result.object_id, "box");
  ASSERT_TRUE(result.source_id.has_value());
  EXPECT_EQ(*result.source_id, "foundationpose");
  ASSERT_TRUE(result.source_age_ns.has_value());
  EXPECT_EQ(*result.source_age_ns, 200);
  EXPECT_EQ(result.details.at("source_freshness_budget_ns"), "99");
}

TEST(SceneSnapshotLeaseValidator, OldPlanningReadingWithFreshLiveSourceAccepts) {
  auto lease = make_lease();
  // Validation long ago relative to the source budget: only the latest
  // reading counts.
  lease.captured_at_ns = 1'000;
  auto context = make_context(lease);
  context.now_ns = 9'000;
  context.live_objects.front().source_timestamp_ns = 8'990;
  context.live_objects.front().source_freshness_budget_ns = 100;

  const auto result = validate_scene_snapshot_lease(lease, context);

  EXPECT_TRUE(result.accepted) << result.reason_code;
}

TEST(SceneSnapshotLeaseValidator, StaleSourceOnObjectAddedAfterPlanningRejects) {
  const auto lease = make_lease();
  auto context = make_context(lease);
  auto added = live_from(make_object("late-box"));
  added.source_id = "late-camera";
  added.source_timestamp_ns = 500;
  added.source_freshness_budget_ns = 100;
  context.live_objects.push_back(std::move(added));

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(result,
                  SceneSnapshotLeaseRejection::kObservedSceneSourceStale,
                  reason_codes::kObservedSceneSourceStale);
  ASSERT_TRUE(result.object_id.has_value());
  EXPECT_EQ(*result.object_id, "late-box");
  ASSERT_TRUE(result.source_id.has_value());
  EXPECT_EQ(*result.source_id, "late-camera");
}

TEST(SceneSnapshotLeaseValidator, FreshObjectAddedAfterPlanningAccepts) {
  const auto lease = make_lease();
  auto context = make_context(lease);
  context.live_world_revision = CollisionWorldRevision{8, "world-hash-v2"};
  ++context.topology_epoch;
  context.live_objects.push_back(live_from(make_object("late-box")));

  const auto result = validate_scene_snapshot_lease(lease, context);

  EXPECT_TRUE(result.accepted) << result.reason_code;
  EXPECT_EQ(result.details.at("accepted_live_topology_epoch_divergence"),
            "true");
}

TEST(SceneSnapshotLeaseValidator, LiveObjectWithoutFreshnessIsNotChecked) {
  const auto lease = make_lease();
  auto context = make_context(lease);
  // An attached object or an authored object without a sensor.
  context.live_objects.front().source_id.reset();
  context.live_objects.front().source_timestamp_ns.reset();
  context.live_objects.front().source_freshness_budget_ns.reset();
  context.now_ns = 9'000;

  const auto result = validate_scene_snapshot_lease(lease, context);

  EXPECT_TRUE(result.accepted) << result.reason_code;
}

TEST(SceneSnapshotLeaseValidator, FutureSourceTimestampRejectsAsClockMismatch) {
  const auto lease = make_lease();
  auto context = make_context(lease);
  context.live_objects.front().source_timestamp_ns = 1'500;
  context.live_objects.front().source_freshness_budget_ns.reset();
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

TEST(SceneSnapshotLeaseValidator, LeaseObjectWithoutBodiesIsStructurallyInvalid) {
  auto lease = make_lease();
  lease.objects.front().collision_body_ids.clear();
  auto context = make_context(lease);

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(result,
                  SceneSnapshotLeaseRejection::kSceneSnapshotStructureInvalid,
                  reason_codes::kSceneSnapshotStructureInvalid);
  EXPECT_EQ(result.details.at("missing_collision_object_identity"), "true");
}

TEST(SceneSnapshotLeaseValidator, DuplicateLeaseObjectIdsReject) {
  auto lease = make_lease();
  lease.objects.push_back(lease.objects.front());
  auto context = make_context(lease);

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(result,
                  SceneSnapshotLeaseRejection::kSceneSnapshotStructureInvalid,
                  reason_codes::kSceneSnapshotStructureInvalid);
  EXPECT_EQ(result.details.at("duplicate_lease_object_id"), "true");
}

TEST(SceneSnapshotLeaseValidator, DuplicateLiveObjectIdsReject) {
  const auto lease = make_lease();
  auto context = make_context(lease);
  context.live_objects.push_back(context.live_objects.front());

  const auto result = validate_scene_snapshot_lease(lease, context);

  expect_rejected(result,
                  SceneSnapshotLeaseRejection::kSceneSnapshotStructureInvalid,
                  reason_codes::kSceneSnapshotStructureInvalid);
  EXPECT_EQ(result.details.at("duplicate_live_object_id"), "true");
}

// Every object change that used to refuse (topology, shape, body hash, pose,
// a missing object, a collision-rule change) is now an accepted newer scene.
TEST(SceneSnapshotLeaseValidator, ChangedObjectsAndRulesAcceptWithRevisionEvidence) {
  auto lease = make_lease();
  lease.objects.push_back(make_object("gone"));
  auto context = make_context(lease);
  context.live_world_revision = CollisionWorldRevision{8, "world-hash-v2"};
  context.live_policy_revision = CollisionPolicyRevision{4, "policy-hash-v2"};
  context.live_projection_revision->scene_revision.revision_id = 9;
  ++context.topology_epoch;
  ++context.policy_epoch;
  context.live_objects.pop_back();
  auto& box = context.live_objects.front();
  box.collision_body_ids.push_back("scene/box/shape/1");
  box.object_revision_token = "box-shape-v2";
  box.body_content_hashes.begin()->second = "box-body-hash-v2";
  box.pose = pose_at(0.250);

  const auto result = validate_scene_snapshot_lease(lease, context);

  EXPECT_TRUE(result.accepted) << result.reason_code;
  EXPECT_EQ(result.rejection, SceneSnapshotLeaseRejection::kNone);
  EXPECT_EQ(result.details.at("accepted_live_scene_revision_divergence"), "true");
  EXPECT_EQ(result.details.at("accepted_live_world_revision_divergence"), "true");
  EXPECT_EQ(result.details.at("accepted_live_policy_revision_divergence"), "true");
  EXPECT_EQ(result.details.at("accepted_live_topology_epoch_divergence"), "true");
  EXPECT_EQ(result.details.at("validated_policy_epoch"), "5");
  EXPECT_EQ(result.details.at("live_policy_epoch"), "6");
  EXPECT_FALSE(has_object_divergence_detail(result));
}

TEST(SceneSnapshotLeaseValidator, ObjectDivergenceDetailOnlyWhenDebugFlagIsSet) {
  auto lease = make_lease();
  lease.objects.push_back(make_object("gone"));
  lease.objects.push_back(make_object("still"));
  auto context = make_context(lease);
  context.live_world_revision = CollisionWorldRevision{8, "world-hash-v2"};
  context.live_objects.erase(context.live_objects.begin() + 1);
  auto& box = context.live_objects.front();
  box.collision_body_ids.push_back("scene/box/shape/1");
  box.object_revision_token = "box-shape-v2";
  box.body_content_hashes.begin()->second = "box-body-hash-v2";
  box.pose = pose_at(0.250);

  const auto off = validate_scene_snapshot_lease(lease, context);
  EXPECT_TRUE(off.accepted) << off.reason_code;
  EXPECT_FALSE(has_object_divergence_detail(off));

  context.record_object_divergence = true;
  const auto on = validate_scene_snapshot_lease(lease, context);
  EXPECT_TRUE(on.accepted) << on.reason_code;
  EXPECT_EQ(on.details.at("object_divergence.gone.missing"), "true");
  EXPECT_EQ(on.details.at("object_divergence.box.bodies"), "true");
  EXPECT_EQ(on.details.at("object_divergence.box.token"), "true");
  EXPECT_EQ(on.details.at("object_divergence.box.body_hash"), "scene/box/shape/0");
  EXPECT_NEAR(std::stod(on.details.at("object_divergence.box.pose_delta_m")), 0.250,
              1e-12);
  EXPECT_FALSE(on.details.contains("object_divergence_truncated_count"));
  for (const auto& [key, value] : on.details) {
    (void)value;
    EXPECT_FALSE(key.starts_with("object_divergence.still.")) << key;
  }
  // Without the per-object keys the two results carry the same evidence.
  auto revision_only = on.details;
  std::erase_if(revision_only, [](const auto& entry) {
    return entry.first.starts_with("object_divergence");
  });
  EXPECT_EQ(revision_only, off.details);
}

TEST(SceneSnapshotLeaseValidator, ObjectDivergenceDetailIsCapped) {
  auto lease = make_lease();
  lease.objects.clear();
  for (int i = 0; i < 20; ++i) {
    lease.objects.push_back(make_object("box-" + std::to_string(i)));
  }
  auto context = make_context(lease);
  for (auto& live : context.live_objects) {
    live.pose = pose_at(0.010);
  }
  context.record_object_divergence = true;

  const auto result = validate_scene_snapshot_lease(lease, context);

  EXPECT_TRUE(result.accepted) << result.reason_code;
  std::size_t recorded = 0;
  for (const auto& [key, value] : result.details) {
    (void)value;
    if (key.starts_with("object_divergence.") && key.ends_with(".pose_delta_m")) {
      ++recorded;
    }
  }
  EXPECT_EQ(recorded, 16U);
  EXPECT_EQ(result.details.at("object_divergence_truncated_count"), "4");
}

}  // namespace
}  // namespace manyforge::core
