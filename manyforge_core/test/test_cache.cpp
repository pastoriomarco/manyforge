#include <gtest/gtest.h>

#include <manyforge_core/cache/trajectory_cache.hpp>
#include <manyforge_core/cache/trajectory_cache_intent.hpp>
#include <manyforge_core/testing/addressed_request_fixtures.hpp>
#include <optional>
#include <set>
#include <utility>

namespace manyforge::core {
namespace {

CollisionWorldRevision make_revision(uint64_t id = 1) {
  CollisionWorldRevision revision;
  revision.revision_id = id;
  revision.content_hash = "test_hash_" + std::to_string(id);
  return revision;
}

TrajectoryDTO make_trajectory(const std::string &label) {
  TrajectoryDTO t;
  t.joint_names = {label};
  TrajectoryWaypointDTO wp;
  wp.time_from_start_s = 0.0;
  wp.positions_rad = {1.0, 2.0};
  t.waypoints.push_back(wp);
  return t;
}

Pose3DTO make_pose(double x, double y, double z) {
  Pose3DTO pose;
  pose.position_m = {x, y, z};
  pose.orientation = {0.0, 0.0, 0.0, 1.0};
  return pose;
}

ExecuteRequestDTO make_cartesian_request() {
  auto request = testing::make_test_execute_request();
  request.move_id = "move";
  std::get<ResolvedComponentModelFrameDTO>(request.binding.tcp_frame)
      .compiled_frame_name = "tool";
  request.tolerances = {0.01, 0.001, 0.01};
  request.speed_profile = {0.5, 0.5, 0.2};
  request.contact_policy_id = "free_space";
  CartesianPathDTO path;
  path.waypoints = {make_pose(0.0, 0.0, 0.0), make_pose(0.1, 0.0, 0.0),
                    make_pose(0.2, 0.0, 0.0)};
  request.motion = std::move(path);
  return request;
}

class TrajectoryCacheTest : public ::testing::Test {
protected:
  static TrajectoryCacheKey key(const std::string &move_id) {
    auto request = testing::make_test_execute_request({"joint1", "joint2"},
                                                      "run:cache-test");
    request.move_id = move_id;
    return trajectory_cache_key(request);
  }

  static TrajectoryCacheRevisionEvidence
  evidence(CollisionWorldRevision revision) {
    return {testing::make_test_projection_binding().projection_revision,
            std::move(revision)};
  }

  void store(const std::string &move_id, TrajectoryDTO trajectory,
             CollisionWorldRevision revision,
             std::string intent_fingerprint = "intent-a") {
    cache.store(key(move_id), std::move(trajectory),
                evidence(std::move(revision)), std::move(intent_fingerprint));
  }

  std::optional<TrajectoryDTO>
  lookup(const std::string &move_id, const CollisionWorldRevision &revision,
         const std::string &intent_fingerprint = "intent-a") {
    return cache
        .lookup_with_revision(key(move_id), evidence(revision),
                              intent_fingerprint)
        .trajectory;
  }

  TrajectoryCache cache{CacheConfig{3}}; // max 3 entries
};

// --- Store and lookup ---

TEST_F(TrajectoryCacheTest, StoreAndLookup) {
  store("move_1", make_trajectory("t1"), make_revision());
  auto result = cache.peek_unvalidated(key("move_1"));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->joint_names[0], "t1");
  EXPECT_EQ(result->waypoints.size(), 1u);
}

TEST_F(TrajectoryCacheTest, LookupMiss_ReturnsNullopt) {
  auto result = cache.peek_unvalidated(key("nonexistent"));
  EXPECT_FALSE(result.has_value());
}

// --- Overwrite ---
TEST_F(TrajectoryCacheTest, StoreOverwritesExisting) {
  store("move_1", make_trajectory("v1"), make_revision());
  store("move_1", make_trajectory("v2"), make_revision());
  auto result = cache.peek_unvalidated(key("move_1"));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->joint_names[0], "v2");
  EXPECT_EQ(cache.size(), 1u);
}

// --- Invalidate ---
TEST_F(TrajectoryCacheTest, InvalidateSpecificMoveId) {
  store("move_1", make_trajectory("t1"), make_revision());
  store("move_2", make_trajectory("t2"), make_revision());
  cache.invalidate(key("move_1"));
  EXPECT_FALSE(cache.peek_unvalidated(key("move_1")).has_value());
  EXPECT_TRUE(cache.peek_unvalidated(key("move_2")).has_value());
  EXPECT_EQ(cache.size(), 1u);
}

TEST_F(TrajectoryCacheTest, InvalidateNonExistent_NoOp) {
  store("move_1", make_trajectory("t1"), make_revision());
  cache.invalidate(key("nonexistent"));
  EXPECT_EQ(cache.size(), 1u);
}

TEST_F(TrajectoryCacheTest, InvalidateAll) {
  store("move_1", make_trajectory("t1"), make_revision());
  store("move_2", make_trajectory("t2"), make_revision());
  cache.invalidate_all();
  EXPECT_EQ(cache.size(), 0u);
  EXPECT_FALSE(cache.peek_unvalidated(key("move_1")).has_value());
  EXPECT_FALSE(cache.peek_unvalidated(key("move_2")).has_value());
}

// --- Multiple independent entries ---
TEST_F(TrajectoryCacheTest, MultipleMovesIndependent) {
  store("move_1", make_trajectory("t1"), make_revision());
  store("move_2", make_trajectory("t2"), make_revision());
  store("move_3", make_trajectory("t3"), make_revision());

  EXPECT_EQ(cache.size(), 3u);
  EXPECT_EQ(cache.peek_unvalidated(key("move_1"))->joint_names[0], "t1");
  EXPECT_EQ(cache.peek_unvalidated(key("move_2"))->joint_names[0], "t2");
  EXPECT_EQ(cache.peek_unvalidated(key("move_3"))->joint_names[0], "t3");
}

TEST_F(TrajectoryCacheTest, EveryCompositeSlotDimensionPreventsAliasing) {
  auto baseline =
      testing::make_test_execute_request({"joint1", "joint2"}, "run:slot-a");
  baseline.move_id = "move";
  std::vector<ExecuteRequestDTO> requests{baseline};

  auto changed = baseline;
  changed.binding.program_revision = ProgramRevision("program-revision:other");
  requests.push_back(changed);
  changed = baseline;
  changed.binding.program_binding_id =
      ProgramBindingId("program-binding:other");
  requests.push_back(changed);
  changed = baseline;
  changed.binding.program_run_id = ProgramRunId("run:slot-b");
  requests.push_back(changed);
  changed = baseline;
  changed.move_id = "move-other";
  requests.push_back(changed);
  changed = baseline;
  changed.binding.controllable_component_id =
      ControllableComponentId("component:other");
  requests.push_back(changed);
  changed = baseline;
  changed.binding.command_target_id = CommandTargetId("target:other");
  requests.push_back(changed);
  changed = baseline;
  changed.binding.capability_id = CapabilityId("execution.trajectory.other");
  requests.push_back(changed);

  TrajectoryCache slot_cache{CacheConfig{requests.size()}};
  for (std::size_t index = 0; index < requests.size(); ++index) {
    slot_cache.store(trajectory_cache_key(requests[index]),
                     make_trajectory("slot-" + std::to_string(index)),
                     evidence(make_revision()),
                     "intent-" + std::to_string(index));
  }

  ASSERT_EQ(slot_cache.size(), requests.size());
  for (std::size_t index = 0; index < requests.size(); ++index) {
    const auto stored =
        slot_cache.peek_unvalidated(trajectory_cache_key(requests[index]));
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->joint_names.front(), "slot-" + std::to_string(index));
  }
}

// --- Size and capacity ---
TEST_F(TrajectoryCacheTest, SizeCorrect) {
  EXPECT_EQ(cache.size(), 0u);
  store("a", make_trajectory("a"), make_revision());
  EXPECT_EQ(cache.size(), 1u);
  store("b", make_trajectory("b"), make_revision());
  EXPECT_EQ(cache.size(), 2u);
}

TEST_F(TrajectoryCacheTest, CapacityFromConfig) {
  EXPECT_EQ(cache.capacity(), 3u);
  TrajectoryCache big_cache{CacheConfig{500}};
  EXPECT_EQ(big_cache.capacity(), 500u);
}

TEST_F(TrajectoryCacheTest, DefaultCapacity) {
  TrajectoryCache default_cache;
  EXPECT_EQ(default_cache.capacity(), 1000u);
}

// --- Safety bound: FIFO eviction ---
TEST_F(TrajectoryCacheTest, EvictsOldestWhenFull) {
  store("move_1", make_trajectory("t1"), make_revision());
  store("move_2", make_trajectory("t2"), make_revision());
  store("move_3", make_trajectory("t3"), make_revision());
  EXPECT_EQ(cache.size(), 3u);

  // 4th entry evicts oldest (move_1).
  store("move_4", make_trajectory("t4"), make_revision());
  EXPECT_EQ(cache.size(), 3u);
  EXPECT_FALSE(cache.peek_unvalidated(key("move_1")).has_value());
  EXPECT_TRUE(cache.peek_unvalidated(key("move_2")).has_value());
  EXPECT_TRUE(cache.peek_unvalidated(key("move_3")).has_value());
  EXPECT_TRUE(cache.peek_unvalidated(key("move_4")).has_value());
}

TEST_F(TrajectoryCacheTest, OverwriteDoesNotEvict) {
  store("move_1", make_trajectory("t1"), make_revision());
  store("move_2", make_trajectory("t2"), make_revision());
  store("move_3", make_trajectory("t3"), make_revision());

  // Overwrite move_1 — should not evict anything.
  store("move_1", make_trajectory("t1_updated"), make_revision());
  EXPECT_EQ(cache.size(), 3u);
  EXPECT_EQ(cache.peek_unvalidated(key("move_1"))->joint_names[0],
            "t1_updated");
  EXPECT_TRUE(cache.peek_unvalidated(key("move_2")).has_value());
  EXPECT_TRUE(cache.peek_unvalidated(key("move_3")).has_value());
}

// --- Empty trajectory ---
// --- Chunk 2: revision-aware cache ---
TEST_F(TrajectoryCacheTest, RevisionAware_StoreAndLookupSucceedsOnExactMatch) {
  CollisionWorldRevision rev{7, "abc"};
  store("move_1", make_trajectory("t1"), rev);
  auto result = lookup("move_1", rev);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->joint_names[0], "t1");
}

TEST_F(TrajectoryCacheTest, RevisionAware_MismatchRevisionReturnsCandidate) {
  CollisionWorldRevision rev_old{7, "abc"};
  CollisionWorldRevision rev_new{8, "def"};
  store("move_1", make_trajectory("t1"), rev_old);

  const auto candidate =
      cache.lookup_with_revision(key("move_1"), evidence(rev_new), "intent-a");
  EXPECT_EQ(candidate.disposition,
            TrajectoryCacheLookupDisposition::kRevalidationCandidate);
  ASSERT_TRUE(candidate.trajectory.has_value());
  EXPECT_EQ(candidate.trajectory->joint_names[0], "t1");
  EXPECT_TRUE(cache.peek_unvalidated(key("move_1")).has_value());
  EXPECT_EQ(cache.size(), 1u);
}

TEST_F(TrajectoryCacheTest, RevisionAware_DifferentHashSameIdEvictsFailClosed) {
  CollisionWorldRevision rev_a{7, "abc"};
  CollisionWorldRevision rev_b{7, "xyz"};
  store("move_1", make_trajectory("t1"), rev_a);

  const auto result =
      cache.lookup_with_revision(key("move_1"), evidence(rev_b), "intent-a");
  EXPECT_EQ(result.disposition,
            TrajectoryCacheLookupDisposition::kRevisionMismatch);
  EXPECT_FALSE(result.trajectory.has_value());
  EXPECT_EQ(cache.size(), 0u);
}

TEST_F(TrajectoryCacheTest, RevisionAware_RevisionRollbackEvictsFailClosed) {
  const CollisionWorldRevision rev_newer{8, "def"};
  const CollisionWorldRevision rev_older{7, "abc"};
  store("move_1", make_trajectory("t1"), rev_newer);

  const auto result = cache.lookup_with_revision(
      key("move_1"), evidence(rev_older), "intent-a");
  EXPECT_EQ(result.disposition,
            TrajectoryCacheLookupDisposition::kRevisionMismatch);
  EXPECT_FALSE(result.trajectory.has_value());
  EXPECT_EQ(cache.size(), 0u);
}

TEST_F(TrajectoryCacheTest,
       RevisionAware_ForwardProjectionRevisionIsRevalidationCandidate) {
  const CollisionWorldRevision world{7, "abc"};
  const auto stored_evidence = evidence(world);
  auto advanced_evidence = stored_evidence;
  advanced_evidence.projection_revision.scene_revision.revision_id += 1U;
  advanced_evidence.projection_revision.scene_revision.content_hash =
      ContentDigest("sha256:" + std::string(64U, '3'));
  cache.store(key("move_1"), make_trajectory("t1"), stored_evidence,
              "intent-a");

  const auto result =
      cache.lookup_with_revision(key("move_1"), advanced_evidence, "intent-a");

  EXPECT_EQ(result.disposition,
            TrajectoryCacheLookupDisposition::kRevalidationCandidate);
  ASSERT_TRUE(result.trajectory.has_value());
  EXPECT_EQ(result.trajectory->joint_names.front(), "t1");
  EXPECT_EQ(cache.size(), 1U);
}

TEST_F(TrajectoryCacheTest,
       RevisionAware_ConflictingProjectionContentEvictsFailClosed) {
  const CollisionWorldRevision world{7, "abc"};
  const auto stored_evidence = evidence(world);
  auto conflicting_evidence = stored_evidence;
  conflicting_evidence.projection_revision.scene_revision.content_hash =
      ContentDigest("sha256:" + std::string(64U, '4'));
  cache.store(key("move_1"), make_trajectory("t1"), stored_evidence,
              "intent-a");

  const auto result = cache.lookup_with_revision(
      key("move_1"), conflicting_evidence, "intent-a");

  EXPECT_EQ(result.disposition,
            TrajectoryCacheLookupDisposition::kRevisionMismatch);
  EXPECT_FALSE(result.trajectory.has_value());
  EXPECT_EQ(cache.size(), 0U);
}

TEST_F(TrajectoryCacheTest,
       RevisionAware_ForeignProjectionLineageEvictsFailClosed) {
  const CollisionWorldRevision world{7, "abc"};
  const auto stored_evidence = evidence(world);
  auto foreign_evidence = stored_evidence;
  foreign_evidence.projection_revision.graph_revision =
      GraphRevision("graph:foreign");
  foreign_evidence.projection_revision.scene_revision.revision_id += 1U;
  foreign_evidence.projection_revision.scene_revision.content_hash =
      ContentDigest("sha256:" + std::string(64U, '5'));
  cache.store(key("move_1"), make_trajectory("t1"), stored_evidence,
              "intent-a");

  const auto result =
      cache.lookup_with_revision(key("move_1"), foreign_evidence, "intent-a");

  EXPECT_EQ(result.disposition,
            TrajectoryCacheLookupDisposition::kRevisionMismatch);
  EXPECT_FALSE(result.trajectory.has_value());
  EXPECT_EQ(cache.size(), 0U);
}

TEST_F(TrajectoryCacheTest, IntentMismatchMissesAndEvicts) {
  const CollisionWorldRevision revision{7, "abc"};
  store("move_1", make_trajectory("t1"), revision, "intent-a");

  const auto result =
      cache.lookup_with_revision(key("move_1"), evidence(revision), "intent-b");
  EXPECT_EQ(result.disposition,
            TrajectoryCacheLookupDisposition::kIntentMismatch);
  EXPECT_FALSE(result.trajectory.has_value());
  EXPECT_EQ(cache.size(), 0u);
}

TEST(TrajectoryCacheIntentTest, EqualResolvedRequestsAreStable) {
  const auto request = make_cartesian_request();
  const auto identity = trajectory_cache_intent_identity(request);
  EXPECT_EQ(identity, trajectory_cache_intent_identity(request));
  EXPECT_GT(identity.size(), 16U);
}

TEST(TrajectoryCacheIntentTest, JointGoalOrderingRemainsPartOfIdentity) {
  auto lhs = make_cartesian_request();
  lhs.motion =
      ResolvedJointGoalDTO{{ResourceId("resource_a"), ResourceId("resource_b")},
                           {"joint_a", "joint_b"},
                           {0.2, -0.4}};
  auto rhs = lhs;
  rhs.motion =
      ResolvedJointGoalDTO{{ResourceId("resource_b"), ResourceId("resource_a")},
                           {"joint_b", "joint_a"},
                           {-0.4, 0.2}};

  EXPECT_NE(trajectory_cache_intent_identity(lhs),
            trajectory_cache_intent_identity(rhs));
}

TEST(TrajectoryCacheIntentTest,
     MalformedJointGoalExtraPositionCannotAliasAnotherRequest) {
  auto lhs = make_cartesian_request();
  lhs.motion =
      ResolvedJointGoalDTO{{ResourceId("resource_a")}, {"joint_a"}, {0.2, 1.0}};
  auto rhs = lhs;
  std::get<ResolvedJointGoalDTO>(rhs.motion).positions_rad[1] = 2.0;

  EXPECT_NE(trajectory_cache_intent_identity(lhs),
            trajectory_cache_intent_identity(rhs));
}

TEST(TrajectoryCacheIntentTest,
     CartesianIntermediateWaypointChangesIdentityWithSameEndpoint) {
  auto lhs = make_cartesian_request();
  auto rhs = lhs;
  std::get<CartesianPathDTO>(rhs.motion).waypoints[1] =
      make_pose(0.1, 0.05, 0.0);

  ASSERT_EQ(
      std::get<CartesianPathDTO>(lhs.motion).waypoints.back().position_m.x_m,
      std::get<CartesianPathDTO>(rhs.motion).waypoints.back().position_m.x_m);
  EXPECT_NE(trajectory_cache_intent_identity(lhs),
            trajectory_cache_intent_identity(rhs));
}

TEST(TrajectoryCacheIntentTest, GoalFrameAndReferenceRobotChangeIdentity) {
  const auto baseline = make_cartesian_request();
  auto own_frame = baseline;
  own_frame.goal_frame_reference = ReferenceDTO(ReferenceDTO::Type::kFrameRole, "tcp");
  auto peer_frame = baseline;
  peer_frame.goal_frame_reference =
      ReferenceDTO(FrameRoleId("tcp"),
                   ReferenceTargetDTO{TargetRoleId("peer_arm"),
                                      ControllableComponentId("peer"), CommandTargetId("arm")});
  auto cell_frame = baseline;
  cell_frame.goal_frame_reference = ReferenceDTO(ReferenceDTO::Type::kCellFrame, "");

  const std::set<std::string> identities{
      trajectory_cache_intent_identity(baseline), trajectory_cache_intent_identity(own_frame),
      trajectory_cache_intent_identity(peer_frame), trajectory_cache_intent_identity(cell_frame)};
  EXPECT_EQ(identities.size(), 4U);
}

TEST(TrajectoryCacheIntentTest, ContactPolicyChangesIdentity) {
  auto lhs = make_cartesian_request();
  auto rhs = lhs;
  rhs.contact_policy_id = "contact_grasp";

  EXPECT_NE(trajectory_cache_intent_identity(lhs),
            trajectory_cache_intent_identity(rhs));
}

TEST(TrajectoryCacheIntentTest, CanonicalModelBindingChangesIdentity) {
  const auto baseline = make_cartesian_request();
  const auto baseline_identity = trajectory_cache_intent_identity(baseline);

  for (const auto &changed : {
           [&] {
             auto request = baseline;
             request.binding.controllable_component_id =
                 ControllableComponentId("component:other");
             return request;
           }(),
           [&] {
             auto request = baseline;
             request.binding.model_group_binding.compiled_group_name =
                 "secondary_arm";
             return request;
           }(),
           [&] {
             auto request = baseline;
             std::get<ResolvedComponentModelFrameDTO>(
                 request.binding.base_frame)
                 .compiled_frame_name = "mobile_base";
             return request;
           }(),
           [&] {
             auto request = baseline;
             std::get<ResolvedComponentModelFrameDTO>(request.binding.tcp_frame)
                 .compiled_frame_name = "alternate_tcp";
             return request;
           }(),
       }) {
    EXPECT_NE(baseline_identity, trajectory_cache_intent_identity(changed));
  }
}

TEST(TrajectoryCacheIntentTest, TargetModeReferenceAndProfileChangeIdentity) {
  auto baseline = make_cartesian_request();
  auto relative = baseline;
  relative.target_mode = MotionTargetModeDTO::kRelative;
  relative.target_reference =
      ReferenceDTO(ReferenceDTO::Type::kFrameRole, "fixture");
  auto balanced = baseline;
  balanced.planning_profile = PlanningProfileDTO::kBalanced;

  const auto baseline_identity = trajectory_cache_intent_identity(baseline);
  EXPECT_NE(baseline_identity, trajectory_cache_intent_identity(relative));
  EXPECT_NE(baseline_identity, trajectory_cache_intent_identity(balanced));
}

TEST(TrajectoryCacheIntentTest,
     RuntimeObjectAdmissionBindingChangesRelativeMotionIdentity) {
  auto baseline = make_cartesian_request();
  baseline.target_mode = MotionTargetModeDTO::kRelative;
  baseline.target_reference =
      ReferenceDTO(ReferenceDTO::Type::kRuntimeObject, "selected-part");
  baseline.runtime_object_bindings = {
      {RuntimeObjectId("selected-part"), SceneResourceId("part-a")}};
  auto rebound = baseline;
  rebound.runtime_object_bindings.front().scene_resource_id =
      SceneResourceId("part-b");

  EXPECT_NE(trajectory_cache_intent_identity(baseline),
            trajectory_cache_intent_identity(rebound));
}

TEST(TrajectoryCacheIntentTest, DynamicsAndCartesianOptionsChangeIdentity) {
  auto baseline = make_cartesian_request();
  auto capped = baseline;
  JointDynamicsLimitDTO limit;
  limit.max_velocity_rad_s = 0.4;
  capped.per_resource_dynamics_limits.push_back(
      ResolvedResourceDynamicsLimitDTO{
          capped.binding.resources.front(),
          capped.binding.model_group_binding.compiled_joint_names.front(),
          limit});
  auto slower_cartesian = baseline;
  slower_cartesian.speed_profile.max_tcp_linear_speed_mps = 0.05;
  auto tighter_blend = baseline;
  std::get<CartesianPathDTO>(tighter_blend.motion).max_blend_deviation_rad =
      0.005;

  const auto baseline_identity = trajectory_cache_intent_identity(baseline);
  EXPECT_NE(baseline_identity, trajectory_cache_intent_identity(capped));
  EXPECT_NE(baseline_identity,
            trajectory_cache_intent_identity(slower_cartesian));
  EXPECT_NE(baseline_identity,
            trajectory_cache_intent_identity(tighter_blend));
}

TEST(TrajectoryCacheIntentTest,
     PlanningBudgetChangesIdentityButExecutionBudgetDoesNot) {
  auto baseline = make_cartesian_request();
  baseline.timeouts = {2.0, 10.0};
  auto planning_budget = baseline;
  planning_budget.timeouts.planning_timeout_s = 4.0;
  auto execution_budget = baseline;
  execution_budget.timeouts.execution_timeout_s = 30.0;

  const auto baseline_identity = trajectory_cache_intent_identity(baseline);
  EXPECT_NE(baseline_identity,
            trajectory_cache_intent_identity(planning_budget));
  EXPECT_EQ(baseline_identity,
            trajectory_cache_intent_identity(execution_budget));
}

TEST_F(TrajectoryCacheTest, StoreRejectsIncompleteProjectionEvidence) {
  EXPECT_THROW(store("move_1", make_trajectory("t1"), CollisionWorldRevision{}),
               std::invalid_argument);
  EXPECT_EQ(cache.size(), 0U);
}

TEST_F(TrajectoryCacheTest, RevisionAware_ExpectedUnsetButStoredTaggedMisses) {
  CollisionWorldRevision rev{7, "abc"};
  store("move_1", make_trajectory("t1"), rev);

  // Caller without an authoritative revision MUST NOT see the tagged hit.
  EXPECT_FALSE(lookup("move_1", CollisionWorldRevision{}).has_value());

  // Entry NOT evicted: it remains valid for a future caller that does
  // supply the correct revision.
  EXPECT_EQ(cache.size(), 1u);
  auto subsequent = lookup("move_1", rev);
  ASSERT_TRUE(subsequent.has_value());
}

TEST_F(TrajectoryCacheTest, RevisionAware_RevisionOfReturnsTaggedValue) {
  CollisionWorldRevision rev{7, "abc"};
  store("move_1", make_trajectory("t1"), rev);
  auto stored = cache.revision_of(key("move_1"));
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->collision_world_revision.revision_id, 7u);
  EXPECT_EQ(stored->collision_world_revision.content_hash, "abc");
  EXPECT_EQ(stored->projection_revision,
            testing::make_test_projection_binding().projection_revision);
}

TEST_F(TrajectoryCacheTest, RevisionAware_OverwriteUpdatesRevision) {
  CollisionWorldRevision rev_a{7, "abc"};
  CollisionWorldRevision rev_b{8, "def"};
  store("move_1", make_trajectory("t1"), rev_a);
  store("move_1", make_trajectory("t1_updated"), rev_b);

  const auto exact =
      cache.lookup_with_revision(key("move_1"), evidence(rev_b), "intent-a");
  EXPECT_EQ(exact.disposition, TrajectoryCacheLookupDisposition::kHit);
  ASSERT_TRUE(exact.trajectory.has_value());
  EXPECT_EQ(exact.trajectory->joint_names[0], "t1_updated");

  const auto rollback =
      cache.lookup_with_revision(key("move_1"), evidence(rev_a), "intent-a");
  EXPECT_EQ(rollback.disposition,
            TrajectoryCacheLookupDisposition::kRevisionMismatch);
  EXPECT_FALSE(rollback.trajectory.has_value());
  EXPECT_EQ(cache.size(), 0u);
}

TEST_F(TrajectoryCacheTest, StoreEmptyTrajectory) {
  TrajectoryDTO empty;
  EXPECT_TRUE(empty.empty());
  store("move_empty", empty, make_revision());
  auto result = cache.peek_unvalidated(key("move_empty"));
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->empty());
}

} // namespace
} // namespace manyforge::core
