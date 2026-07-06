// Chunk 5 unit tests for CollisionValidationCoordinator: trajectory-wide
// validation, support-contact state across waypoints, fail-closed
// preservation, and pose-source borrow/retry behavior.

#include <manyforge_core/collision/collision_validation_coordinator.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <map>
#include <queue>
#include <vector>

namespace manyforge::core {
namespace {


class MockCollisionQueryEngine : public ICollisionQueryEngine {
 public:
  CollisionResult default_result;
  std::map<std::int64_t, CollisionResult> result_by_waypoint;
  std::function<void(const CollisionQuery&)> after_check;
  int check_count = 0;

  MockCollisionQueryEngine() {
    default_result.in_collision = false;
    default_result.engine_status = CollisionEngineStatus::kOk;
    default_result.decision = CollisionDecisionResult::kPass;
    default_result.engine_id = "mock_engine";
    default_result.engine_version = "test";
    capability_.engine_id = "mock_engine";
    capability_.engine_version = "test";
    capability_.library_lineage = "unit_test";
    capability_.supported_geometry_kinds = {"box"};
    capability_.thread_safety =
        CollisionEngineThreadSafetyClass::kSingleContextExclusive;
    capability_.deterministic_ordering = true;
  }

  const CollisionEngineCapabilityDescriptor& capability() const override {
    return capability_;
  }

  CollisionResult check(const CollisionWorldSnapshot& snapshot,
                        const CollisionQuery& query,
                        const CollisionPolicySnapshot&,
                        const CollisionCancellationToken& cancel) override {
    ++check_count;
    CollisionResult result = default_result;
    if (query.waypoint_index) {
      auto it = result_by_waypoint.find(*query.waypoint_index);
      if (it != result_by_waypoint.end()) {
        result = it->second;
      }
    }
    result.query_id = query.query_id;
    result.surface = query.surface;
    result.state_time_ns = query.state_time_ns;
    result.collision_world_revision = snapshot.revision;
    result.engine_id = capability_.engine_id;
    result.engine_version = capability_.engine_version;
    if (cancel.is_cancel_requested()) {
      result.in_collision = false;
      result.normalized_pairs.clear();
      result.engine_status = CollisionEngineStatus::kCancelled;
      result.decision = CollisionDecisionResult::kInconclusiveFail;
      result.raw_diagnostics["cancel_reason"] = cancel.reason();
    }
    if (after_check) {
      after_check(query);
    }
    return result;
  }

  std::string engine_id() const override { return capability_.engine_id; }
  std::string engine_version() const override {
    return capability_.engine_version;
  }

 private:
  CollisionEngineCapabilityDescriptor capability_;
};

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

CollisionResult make_cancelled_result(std::string reason) {
  CollisionResult r;
  r.in_collision = false;
  r.engine_status = CollisionEngineStatus::kCancelled;
  r.decision = CollisionDecisionResult::kInconclusiveFail;
  r.engine_id = "mock_engine";
  r.engine_version = "0.x";
  r.raw_diagnostics["cancel_reason"] = std::move(reason);
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

CollisionResult make_pass_result_with_clearance(const CollisionPairId& pair,
                                                 double signed_clearance_m) {
  CollisionResult r;
  r.in_collision = signed_clearance_m < 0.0;
  if (r.in_collision) {
    r.normalized_pairs = {pair};
  }
  r.pair_clearances = {CollisionPairClearance{pair, signed_clearance_m}};
  r.min_clearance_m = signed_clearance_m;
  r.min_clearance_pair = pair;
  r.engine_status = CollisionEngineStatus::kOk;
  r.decision = r.in_collision ? CollisionDecisionResult::kFail
                              : CollisionDecisionResult::kPass;
  r.engine_id = "mock_engine";
  r.engine_version = "test";
  return r;
}

CollisionResult make_colliding_result_with_clearance(const CollisionPairId& pair,
                                                     double signed_clearance_m) {
  CollisionResult r = make_pass_result_with_clearance(pair, signed_clearance_m);
  r.in_collision = true;
  r.normalized_pairs = {pair};
  r.decision = CollisionDecisionResult::kFail;
  return r;
}

CollisionResult make_pass_result_without_clearance() {
  CollisionResult r;
  r.in_collision = false;
  r.engine_status = CollisionEngineStatus::kOk;
  r.decision = CollisionDecisionResult::kPass;
  r.engine_id = "mock_engine";
  r.engine_version = "test";
  return r;
}

CollisionPolicySnapshot make_contact_release_snapshot(
    const CollisionPairId& pair,
    double max_start_penetration_m,
    bool require_separation_by_end,
    bool require_strictly_decreasing_penetration) {
  CollisionPolicySnapshot s = make_freespace_snapshot();
  SupportContactPolicyEntry entry;
  entry.pair = pair;
  entry.mode = CollisionContactPolicyMode::kContactRelease;
  entry.max_start_penetration_m = max_start_penetration_m;
  entry.release_tolerance_m = 1e-5;
  entry.min_release_progress_m = 0.0;
  entry.min_separation_clearance_m = 0.0;
  entry.require_separation_by_end = require_separation_by_end;
  entry.require_strictly_decreasing_penetration =
      require_strictly_decreasing_penetration;
  entry.allow_recontact = false;
  s.support_contacts.push_back(entry);
  return s;
}

CollisionPolicySnapshot make_contact_process_snapshot(
    const CollisionPairId& pair) {
  CollisionPolicySnapshot s = make_freespace_snapshot();
  SupportContactPolicyEntry entry;
  entry.pair = pair;
  entry.mode = CollisionContactPolicyMode::kContactProcess;
  s.support_contacts.push_back(entry);
  return s;
}

CollisionPolicySnapshot make_contact_grasp_snapshot() {
  CollisionPolicySnapshot s = make_freespace_snapshot();
  s.require_full_diagnostics = true;
  ContactGraspPolicyEntry entry;
  entry.policy_id = "contact_grasp/part";
  entry.selected_object_id = "part";
  entry.allowed_robot_links = {"finger_left", "finger_right"};
  entry.max_penetration_m = 0.003;
  entry.terminal_window_start_fraction = 0.75;
  s.contact_grasps.push_back(entry);
  return s;
}

TrajectoryDTO make_trivial_trajectory(int waypoint_count) {
  TrajectoryDTO t;
  t.joint_names = {"j0"};
  for (int i = 0; i < waypoint_count; ++i) {
    TrajectoryWaypointDTO wp;
    wp.positions_rad.push_back(0.1 * i);
    wp.time_from_start_s = 0.1 * i;
    t.waypoints.push_back(wp);
  }
  return t;
}

CollisionWorldSnapshot make_snapshot() {
  CollisionWorldSnapshot snapshot;
  snapshot.publisher_id = "unit_test";
  snapshot.revision = CollisionWorldRevision{11, "snapshot-11"};
  snapshot.snapshot_format_version = 1;
  snapshot.body_count = 1;
  snapshot.geometry_count = 1;
  snapshot.all_bodies_have_geometry = true;
  return snapshot;
}

TEST(CollisionValidationCoordinatorTest,
     EngineBackedValidationCancelsBeforeFirstWaypoint) {
  MockCollisionQueryEngine engine;
  CollisionCancellationToken cancel;
  cancel.request_cancel("operator_stop");

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_freespace_snapshot();
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(3), cancel);

  EXPECT_TRUE(bundle.cancelled);
  ASSERT_TRUE(bundle.cancellation_reason.has_value());
  EXPECT_EQ(*bundle.cancellation_reason, "operator_stop");
  ASSERT_TRUE(bundle.cancelled_at_waypoint_index.has_value());
  EXPECT_EQ(*bundle.cancelled_at_waypoint_index, 0);
  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  EXPECT_EQ(bundle.decision.severity, CollisionDecisionSeverity::kBlock);
  ASSERT_TRUE(bundle.final_world_revision.has_value());
  EXPECT_EQ(bundle.final_world_revision->revision_id, 11u);
  EXPECT_EQ(bundle.waypoint_records.size(), 0u);
  EXPECT_EQ(engine.check_count, 0);
}

TEST(CollisionValidationCoordinatorTest,
     EngineBackedValidationCarriesEngineCancellationRecord) {
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[1] = make_cancelled_result("engine_cancelled");
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_freespace_snapshot();
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(4), cancel);

  EXPECT_TRUE(bundle.cancelled);
  ASSERT_TRUE(bundle.cancellation_reason.has_value());
  EXPECT_EQ(*bundle.cancellation_reason, "engine_cancelled");
  ASSERT_TRUE(bundle.cancelled_at_waypoint_index.has_value());
  EXPECT_EQ(*bundle.cancelled_at_waypoint_index, 1);
  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 1);
  ASSERT_EQ(bundle.waypoint_records.size(), 2u);
  EXPECT_EQ(bundle.waypoint_records.back().result.engine_status,
            CollisionEngineStatus::kCancelled);
  EXPECT_EQ(engine.check_count, 2);
}

TEST(CollisionValidationCoordinatorTest,
     EngineBackedCollisionBeforeCancellationPreservesRejectDecision) {
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[0] =
      make_colliding_result(CollisionPairId{"robot/link", "object/part"});
  engine.result_by_waypoint[2] = make_cancelled_result("operator_stop");
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_freespace_snapshot();
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(4), cancel);

  EXPECT_TRUE(bundle.cancelled);
  ASSERT_TRUE(bundle.cancelled_at_waypoint_index.has_value());
  EXPECT_EQ(*bundle.cancelled_at_waypoint_index, 2);
  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kReject);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 0);
  ASSERT_EQ(bundle.waypoint_records.size(), 3u);
}

TEST(CollisionValidationCoordinatorTest,
     EngineBackedCancellationCarriesWaypointTimeWhenNoPriorFailure) {
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[2] = make_cancelled_result("operator_stop");
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_freespace_snapshot();
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(4), cancel);

  EXPECT_TRUE(bundle.cancelled);
  ASSERT_TRUE(bundle.cancelled_at_waypoint_index.has_value());
  EXPECT_EQ(*bundle.cancelled_at_waypoint_index, 2);
  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 2);
  ASSERT_TRUE(bundle.decision.first_failing_time_ns.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_time_ns, 200'000'000);
}

TEST(CollisionValidationCoordinatorTest,
     EngineBackedCancelAfterPriorRejectPreservesRejectTimeAndReason) {
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[1] =
      make_colliding_result(CollisionPairId{"robot/link", "object/part"});
  engine.result_by_waypoint[3] = make_cancelled_result("operator_stop");
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_freespace_snapshot();
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(5), cancel);

  EXPECT_TRUE(bundle.cancelled);
  ASSERT_TRUE(bundle.cancellation_reason.has_value());
  EXPECT_EQ(*bundle.cancellation_reason, "operator_stop");
  ASSERT_TRUE(bundle.cancelled_at_waypoint_index.has_value());
  EXPECT_EQ(*bundle.cancelled_at_waypoint_index, 3);
  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kReject);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 1);
  ASSERT_TRUE(bundle.decision.first_failing_time_ns.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_time_ns, 100'000'000);
}

TEST(CollisionValidationCoordinatorTest,
     LateCancelAfterPolicyPostPassCannotEscapeAccepted) {
  MockCollisionQueryEngine engine;
  CollisionCancellationToken cancel;
  engine.after_check = [&cancel](const CollisionQuery& query) {
    if (query.waypoint_index && *query.waypoint_index == 2) {
      cancel.request_cancel("late_operator_stop");
    }
  };

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_freespace_snapshot();
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(3), cancel);

  EXPECT_TRUE(bundle.cancelled);
  ASSERT_TRUE(bundle.cancellation_reason.has_value());
  EXPECT_EQ(*bundle.cancellation_reason, "late_operator_stop");
  ASSERT_TRUE(bundle.cancelled_at_waypoint_index.has_value());
  EXPECT_EQ(*bundle.cancelled_at_waypoint_index, 2);
  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 2);
  ASSERT_TRUE(bundle.decision.first_failing_time_ns.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_time_ns, 200'000'000);
}

// --- Contact-release on the engine-backed mock -----------------

TEST(CollisionValidationCoordinatorTest,
     ContactReleaseFailsClosedWhenNumericClearanceMissing) {
  // Engine returns a pass with no pair_clearances. The contact-release gate
  // requires numeric clearance for the configured pair; absence must produce
  // a kInconclusiveFail with reason "missing_contact_release_clearance".
  CollisionPairId pair{"robot/grasp", "object/box"};
  MockCollisionQueryEngine engine;
  engine.default_result = make_pass_result_without_clearance();
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_contact_release_snapshot(
      pair, /*max_start=*/0.005,
      /*require_separation_by_end=*/false,
      /*require_strictly_decreasing_penetration=*/false);
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(3), cancel);

  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 0);
  ASSERT_FALSE(bundle.waypoint_records.empty());
  EXPECT_EQ(bundle.waypoint_records.front()
                .result.raw_diagnostics.at("contact_release_violation"),
            "missing_contact_release_clearance");
}

TEST(CollisionValidationCoordinatorTest,
     ContactReleaseStrictlyDecreasingPenetrationPasses) {
  // Initial contact with penetration <= max_start, depth monotonically
  // decreases to 0 (clearance crosses positive at the end). Must pass.
  CollisionPairId pair{"robot/grasp", "object/box"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[0] = make_pass_result_with_clearance(pair, -0.003);
  engine.result_by_waypoint[1] = make_pass_result_with_clearance(pair, -0.001);
  engine.result_by_waypoint[2] = make_pass_result_with_clearance(pair, 0.002);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_contact_release_snapshot(
      pair, /*max_start=*/0.005,
      /*require_separation_by_end=*/true,
      /*require_strictly_decreasing_penetration=*/true);
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(3), cancel);

  EXPECT_TRUE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kDispatch);
  EXPECT_FALSE(bundle.decision.first_failing_waypoint.has_value());
}

TEST(CollisionValidationCoordinatorTest,
     ContactReleaseInitialPenetrationExceedsEnvelopeFailsClosed) {
  // Initial depth > max_start_penetration_m + tolerance must reject at
  // waypoint 0 with reason "initial_penetration_exceeds_policy".
  CollisionPairId pair{"robot/grasp", "object/box"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[0] = make_pass_result_with_clearance(pair, -0.020);
  engine.result_by_waypoint[1] = make_pass_result_with_clearance(pair, -0.010);
  engine.result_by_waypoint[2] = make_pass_result_with_clearance(pair, 0.001);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_contact_release_snapshot(
      pair, /*max_start=*/0.005,
      /*require_separation_by_end=*/true,
      /*require_strictly_decreasing_penetration=*/true);
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(3), cancel);

  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kReject);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 0);
  EXPECT_EQ(bundle.waypoint_records.front()
                .result.raw_diagnostics.at("contact_release_violation"),
            "initial_penetration_exceeds_policy");
}

TEST(CollisionValidationCoordinatorTest,
     ContactReleaseInitialPenetrationEnvelopeCanBeDisabled) {
  CollisionPairId pair{"robot/grasp", "object/box"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[0] = make_pass_result_with_clearance(pair, -0.020);
  engine.result_by_waypoint[1] = make_pass_result_with_clearance(pair, -0.010);
  engine.result_by_waypoint[2] = make_pass_result_with_clearance(pair, 0.001);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_contact_release_snapshot(
      pair, /*max_start=*/0.005,
      /*require_separation_by_end=*/true,
      /*require_strictly_decreasing_penetration=*/true);
  cfg.policy_snapshot.support_contacts.front()
      .enforce_max_start_penetration = false;
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(3), cancel);

  EXPECT_TRUE(bundle.decision.accepted);
}

TEST(CollisionValidationCoordinatorTest,
     ContactReleasePenetrationIncreaseFailsAtFirstViolation) {
  // Penetration monotonicity: a later waypoint deeper than previous beyond
  // tolerance must fail with reason "penetration_increased" at the first
  // violating index.
  CollisionPairId pair{"robot/grasp", "object/box"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[0] = make_pass_result_with_clearance(pair, -0.004);
  engine.result_by_waypoint[1] = make_pass_result_with_clearance(pair, -0.003);
  engine.result_by_waypoint[2] = make_pass_result_with_clearance(pair, -0.020);
  engine.result_by_waypoint[3] = make_pass_result_with_clearance(pair, 0.001);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_contact_release_snapshot(
      pair, /*max_start=*/0.005,
      /*require_separation_by_end=*/true,
      /*require_strictly_decreasing_penetration=*/true);
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(4), cancel);

  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kReject);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 2);
  EXPECT_EQ(bundle.waypoint_records[2]
                .result.raw_diagnostics.at("contact_release_violation"),
            "penetration_increased");
}

TEST(CollisionValidationCoordinatorTest,
     ContactReleaseFinalSeparationRequiredFailsWhenStillPenetrating) {
  // Strictly decreasing penetration but no separation by end, with
  // require_separation_by_end=true, must fail at the final waypoint
  // with reason "final_sample_not_separated".
  CollisionPairId pair{"robot/grasp", "object/box"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[0] = make_pass_result_with_clearance(pair, -0.004);
  engine.result_by_waypoint[1] = make_pass_result_with_clearance(pair, -0.002);
  engine.result_by_waypoint[2] = make_pass_result_with_clearance(pair, -0.001);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_contact_release_snapshot(
      pair, /*max_start=*/0.005,
      /*require_separation_by_end=*/true,
      /*require_strictly_decreasing_penetration=*/false);
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(3), cancel);

  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kReject);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 2);
  EXPECT_EQ(bundle.waypoint_records.back()
                .result.raw_diagnostics.at("contact_release_violation"),
            "final_sample_not_separated");
}

TEST(CollisionValidationCoordinatorTest,
     ContactProcessModeFailsClosed) {
  // kContactProcess is reserved fail-closed until force/controller sensing
  // contracts qualify. Validation must reject with
  // "contact_process_policy_not_implemented".
  CollisionPairId pair{"robot/tcp", "object/part"};
  MockCollisionQueryEngine engine;
  engine.default_result = make_pass_result_with_clearance(pair, -0.001);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_contact_process_snapshot(pair);
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(3), cancel);

  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  ASSERT_FALSE(bundle.waypoint_records.empty());
  EXPECT_EQ(bundle.waypoint_records.front()
                .result.raw_diagnostics.at("contact_release_violation"),
            "contact_process_policy_not_implemented");
  EXPECT_EQ(bundle.waypoint_records.front()
                .result.raw_diagnostics.at("contact_release_mode"),
            "contact_process");
}

TEST(CollisionValidationCoordinatorTest,
     ContactGraspAllowsSelectedObjectContactInTerminalWindow) {
  CollisionPairId pair{
      "object/part/shape/0",
      "robot/arm/link/finger_left/collision/0"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[3] =
      make_colliding_result_with_clearance(pair, -0.001);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_contact_grasp_snapshot();
  cfg.contact_policy_id = "contact_grasp/part";
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(4), cancel);

  EXPECT_TRUE(bundle.decision.accepted);
  ASSERT_EQ(bundle.waypoint_records[3].result.normalized_pairs_classified.size(),
            1u);
  EXPECT_EQ(bundle.waypoint_records[3]
                .result.normalized_pairs_classified.front()
                .classification,
            CollisionPairClassification::kAllowedContactGrasp);
}

TEST(CollisionValidationCoordinatorTest,
     ContactGraspRejectsSelectedObjectContactBeforeTerminalWindow) {
  CollisionPairId pair{
      "object/part/shape/0",
      "robot/arm/link/finger_left/collision/0"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[1] =
      make_colliding_result_with_clearance(pair, -0.001);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_contact_grasp_snapshot();
  cfg.contact_policy_id = "contact_grasp/part";
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(4), cancel);

  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kReject);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 1);
  EXPECT_EQ(bundle.waypoint_records[1]
                .result.raw_diagnostics.at("contact_grasp_violation"),
            "contact_before_terminal_window");
}

TEST(CollisionValidationCoordinatorTest,
     ContactGraspUnknownPolicyFailsClosed) {
  MockCollisionQueryEngine engine;
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_contact_grasp_snapshot();
  cfg.contact_policy_id = "contact_grasp/missing";
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(2), cancel);

  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  EXPECT_EQ(bundle.waypoint_records.front()
                .result.raw_diagnostics.at("contact_grasp_violation"),
            "unknown_contact_policy_id");
}


}  // namespace
}  // namespace manyforge::core
