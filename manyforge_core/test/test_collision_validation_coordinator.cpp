// Chunk 5 unit tests for CollisionValidationCoordinator: trajectory-wide
// validation, support-contact state across waypoints, fail-closed
// preservation, and pose-source borrow/retry behavior.

#include <manyforge_core/collision/collision_validation_coordinator.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <queue>
#include <vector>

namespace manyforge::core {
namespace {

class MockPlanningContext : public IPlanningContext {
 public:
  // Queue of normalized results consumed FIFO per collision_check_normalized
  // call. If empty, returns the default `next_normalized`.
  std::queue<CollisionResult> normalized_sequence;
  CollisionResult next_normalized;
  CollisionWorldRevision world_revision{1, "mock-world"};

  // Counter used to flip the revision mid-pass for pose-source tests.
  int revision_bumps_remaining = 0;
  bool revision_bump_on_each_call = false;
  CollisionQuery last_query;

  // Plain unused implementations.
  RobotStateDTO get_current_state() override { return {}; }
  ValidationResultDTO validate_trajectory(const TrajectoryDTO&,
                                          const ValidationOptionsDTO&) override {
    ValidationResultDTO r;
    r.valid = true;
    return r;
  }
  CollisionCheckResultDTO collision_check(
      const CollisionCheckRequestDTO&) override {
    return {};
  }
  Pose3DTO forward_kinematics(const FkRequestDTO&) override { return {}; }
  Pose3DTO get_object_pose(const std::string&, const std::string&) override {
    return {};
  }
  ObjectStateDTO get_object_state(const std::string&) override { return {}; }
  Result<Pose3DTO> resolve_reference_pose(const ReferenceDTO&,
                                          const std::string&) override {
    return Result<Pose3DTO>::Ok(Pose3DTO{});
  }
  JointLimitsDTO get_joint_limits(const std::string&,
                                  const std::string&) override {
    return {};
  }
  SceneEditResultDTO add_object(const AddObjectRequestDTO&) override {
    return {};
  }
  SceneEditResultDTO remove_object(const RemoveObjectRequestDTO&) override {
    return {};
  }
  SceneEditResultDTO update_object_pose(
      const UpdateObjectPoseRequestDTO&) override {
    return {};
  }
  SceneEditResultDTO attach_object(const AttachObjectRequestDTO&) override {
    return {};
  }
  SceneEditResultDTO detach_object(const DetachObjectRequestDTO&) override {
    return {};
  }

  CollisionWorldRevision current_collision_world_revision() override {
    if (revision_bump_on_each_call && revision_bumps_remaining > 0) {
      world_revision.revision_id++;
      world_revision.content_hash =
          "mock-world-r" + std::to_string(world_revision.revision_id);
      revision_bumps_remaining--;
    }
    return world_revision;
  }

  CollisionResult collision_check_normalized(const CollisionQuery& query) override {
    last_query = query;
    if (!normalized_sequence.empty()) {
      auto r = std::move(normalized_sequence.front());
      normalized_sequence.pop();
      return r;
    }
    return next_normalized;
  }
};

CollisionResult make_clean_result() {
  CollisionResult r;
  r.in_collision = false;
  r.engine_status = CollisionEngineStatus::kOk;
  r.decision = CollisionDecisionResult::kPass;
  r.engine_id = "mock_engine";
  r.engine_version = "0.x";
  return r;
}

CollisionResult make_colliding_result(const CollisionPairId& pair) {
  CollisionResult r;
  r.in_collision = true;
  r.normalized_pairs = {pair};
  r.engine_status = CollisionEngineStatus::kOk;
  r.decision = CollisionDecisionResult::kFail;
  r.engine_id = "mock_engine";
  r.engine_version = "0.x";
  return r;
}

CollisionResult make_inconclusive_result() {
  CollisionResult r;
  r.in_collision = false;
  r.engine_status = CollisionEngineStatus::kDiagnosticIncomplete;
  r.decision = CollisionDecisionResult::kInconclusiveFail;
  r.engine_id = "mock_engine";
  r.engine_version = "0.x";
  return r;
}

CollisionPolicySnapshot make_freespace_snapshot() {
  CollisionPolicySnapshot s;
  s.policy_id = "free_space";
  s.revision.revision_id = 7;
  s.revision.content_hash = "pol7";
  s.require_full_diagnostics = false;
  return s;
}

CollisionPolicySnapshot make_degraded_support_snapshot(
    const CollisionPairId& pair) {
  CollisionPolicySnapshot s = make_freespace_snapshot();
  s.require_full_diagnostics = false;
  SupportContactPolicyEntry entry;
  entry.pair = pair;
  entry.require_separation_by_end = true;
  entry.allow_recontact = false;
  s.support_contacts.push_back(entry);
  return s;
}

TrajectoryDTO make_trivial_trajectory(int waypoint_count) {
  TrajectoryDTO t;
  t.joint_names = {"j0"};
  for (int i = 0; i < waypoint_count; ++i) {
    TrajectoryWaypointDTO wp;
    wp.positions_rad = {0.1 * i};
    wp.time_from_start_s = 0.1 * i;
    t.waypoints.push_back(wp);
  }
  return t;
}

// --- Trajectory-wide validation passing -----------------------------------

TEST(CollisionValidationCoordinatorTest, PassesCleanTrajectory) {
  MockPlanningContext ctx;
  ctx.next_normalized = make_clean_result();

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_freespace_snapshot();
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(ctx, make_trivial_trajectory(5));
  EXPECT_TRUE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kDispatch);
  EXPECT_FALSE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(bundle.waypoint_records.size(), 5u);
  EXPECT_EQ(bundle.borrowed_world_revision.revision_id, 1u);
}

// --- Trajectory-wide validation rejecting at first blocking waypoint ------

TEST(CollisionValidationCoordinatorTest,
     RejectsAtLowestFailingWaypointIndex) {
  MockPlanningContext ctx;
  // 5 waypoints: clean, clean, collide, collide, clean.
  ctx.normalized_sequence.push(make_clean_result());
  ctx.normalized_sequence.push(make_clean_result());
  ctx.normalized_sequence.push(
      make_colliding_result(CollisionPairId{"a", "b"}));
  ctx.normalized_sequence.push(
      make_colliding_result(CollisionPairId{"c", "d"}));
  ctx.normalized_sequence.push(make_clean_result());

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_freespace_snapshot();
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(ctx, make_trivial_trajectory(5));
  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kReject);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 2);
}

// --- Diagnostic-incomplete cannot be downgraded to pass --------------------

TEST(CollisionValidationCoordinatorTest,
     DiagnosticIncompleteRemainsBlockingThroughEvaluator) {
  MockPlanningContext ctx;
  ctx.next_normalized = make_inconclusive_result();

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_freespace_snapshot();
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(ctx, make_trivial_trajectory(1));
  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
}

TEST(CollisionValidationCoordinatorTest,
     FullDiagnosticSupportContactRequestsPenetrationDepth) {
  MockPlanningContext ctx;
  ctx.next_normalized = make_clean_result();

  CollisionPolicySnapshot snapshot = make_freespace_snapshot();
  snapshot.require_full_diagnostics = true;
  SupportContactPolicyEntry entry;
  entry.pair = CollisionPairId{"gripper", "part"};
  snapshot.support_contacts.push_back(entry);

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = snapshot;
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(ctx, make_trivial_trajectory(1));
  EXPECT_TRUE(bundle.decision.accepted);
  EXPECT_NE(std::find(ctx.last_query.required_capabilities.begin(),
                      ctx.last_query.required_capabilities.end(),
                      "body_pair_only"),
            ctx.last_query.required_capabilities.end());
  EXPECT_NE(std::find(ctx.last_query.required_capabilities.begin(),
                      ctx.last_query.required_capabilities.end(),
                      "penetration_depth"),
            ctx.last_query.required_capabilities.end());
  ASSERT_TRUE(ctx.last_query.waypoint_index.has_value());
  EXPECT_EQ(*ctx.last_query.waypoint_index, 0);
}

TEST(CollisionValidationCoordinatorTest,
     SupportContactMustSeparateByFinalWaypoint) {
  const CollisionPairId pair{"gripper", "part"};
  MockPlanningContext ctx;
  ctx.normalized_sequence.push(make_colliding_result(pair));
  ctx.normalized_sequence.push(make_colliding_result(pair));
  ctx.normalized_sequence.push(make_colliding_result(pair));

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_degraded_support_snapshot(pair);
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(ctx, make_trivial_trajectory(3));
  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kReject);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 2);
}

TEST(CollisionValidationCoordinatorTest,
     SupportContactSeparationByFinalWaypointPassesWhenDegradedAllowed) {
  const CollisionPairId pair{"gripper", "part"};
  MockPlanningContext ctx;
  ctx.normalized_sequence.push(make_colliding_result(pair));
  ctx.normalized_sequence.push(make_clean_result());
  ctx.normalized_sequence.push(make_clean_result());

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_degraded_support_snapshot(pair);
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(ctx, make_trivial_trajectory(3));
  EXPECT_TRUE(bundle.decision.accepted);
}

// --- Pose-source borrow records final revision in bundle -------------------

TEST(CollisionValidationCoordinatorTest,
     BorrowsRevisionAtStartAndExposesItOnBundle) {
  MockPlanningContext ctx;
  ctx.next_normalized = make_clean_result();
  ctx.world_revision = {42, "mock-r42"};

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_freespace_snapshot();
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(ctx, make_trivial_trajectory(2));
  EXPECT_EQ(bundle.borrowed_world_revision.revision_id, 42u);
  ASSERT_TRUE(bundle.final_world_revision.has_value());
  EXPECT_EQ(bundle.final_world_revision->revision_id, 42u);
}

// --- Pose-source apply mid-pass triggers retry and eventual fail-closed ----

TEST(CollisionValidationCoordinatorTest,
     SceneChangedDuringValidationFailsClosedAfterRetriesExhausted) {
  MockPlanningContext ctx;
  ctx.next_normalized = make_clean_result();
  // Every revision-query bumps the revision so retries can never settle.
  ctx.revision_bump_on_each_call = true;
  ctx.revision_bumps_remaining = 1000;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_freespace_snapshot();
  cfg.pose_source_retry.max_count = 1;
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(ctx, make_trivial_trajectory(2));
  EXPECT_TRUE(bundle.scene_changed_during_validation);
  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  EXPECT_GE(bundle.pose_source_retries_used, 1);
}

TEST(CollisionValidationCoordinatorTest,
     ChurnWindowExceededIsReported) {
  MockPlanningContext ctx;
  ctx.next_normalized = make_clean_result();
  ctx.revision_bump_on_each_call = true;
  ctx.revision_bumps_remaining = 1000;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_freespace_snapshot();
  cfg.pose_source_retry.max_count = 2;
  cfg.pose_source_retry.churn_max_revisions = 1;
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(ctx, make_trivial_trajectory(2));
  EXPECT_TRUE(bundle.scene_changed_during_validation);
  EXPECT_TRUE(bundle.churn_window_exceeded);
  EXPECT_FALSE(bundle.decision.accepted);
}

// --- Disabling borrow skips retry; final decision passes despite churn -----

TEST(CollisionValidationCoordinatorTest,
     DisableBorrowSkipsRetryAndAcceptsChurnedScene) {
  MockPlanningContext ctx;
  ctx.next_normalized = make_clean_result();
  ctx.revision_bump_on_each_call = true;
  ctx.revision_bumps_remaining = 1000;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_freespace_snapshot();
  cfg.require_pose_source_borrow = false;
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(ctx, make_trivial_trajectory(2));
  EXPECT_FALSE(bundle.scene_changed_during_validation);
  EXPECT_TRUE(bundle.decision.accepted);
  EXPECT_EQ(bundle.pose_source_retries_used, 0);
}

// --- Single-state validation convenience hits the same code path ----------

TEST(CollisionValidationCoordinatorTest, ValidateStateWrapsTrajectoryPath) {
  MockPlanningContext ctx;
  ctx.next_normalized = make_clean_result();

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_freespace_snapshot();
  CollisionValidationCoordinator coord(cfg);

  RobotStateDTO state;
  state.joint_names = {"j0"};
  state.positions_rad = {0.0};
  auto bundle = coord.validate_state(ctx, state);
  EXPECT_TRUE(bundle.decision.accepted);
  EXPECT_EQ(bundle.waypoint_records.size(), 1u);
}

// --- Policy revision is echoed onto every classified result ---------------

TEST(CollisionValidationCoordinatorTest, PolicyRevisionAppearsOnWaypoints) {
  MockPlanningContext ctx;
  ctx.next_normalized = make_clean_result();

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_freespace_snapshot();
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(ctx, make_trivial_trajectory(3));
  ASSERT_EQ(bundle.waypoint_records.size(), 3u);
  for (const auto& rec : bundle.waypoint_records) {
    ASSERT_TRUE(rec.result.policy_revision_used.has_value());
    EXPECT_EQ(*rec.result.policy_revision_used, "7");
  }
}

}  // namespace
}  // namespace manyforge::core
