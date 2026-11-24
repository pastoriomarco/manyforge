// Unit tests for CollisionValidationCoordinator: trajectory-wide
// validation, release-rule state across waypoints, fail-closed
// preservation, and pose-source borrow/retry behavior.

#include <manyforge_core/collision/collision_validation_coordinator.hpp>
#include <manyforge_core/execution_collision_support.hpp>

#include <gtest/gtest.h>

#include "collision_rule_test_helpers.hpp"

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
  std::vector<CollisionQuery> observed_queries;
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
    capability_.supports_distance_query = true;
    capability_.supported_distance_request_modes = {
        CollisionDistanceRequestMode::kNone,
        CollisionDistanceRequestMode::kSelectedCollidingPairs,
        CollisionDistanceRequestMode::kAllCollidingPairs,
        CollisionDistanceRequestMode::kAllActivePairs};
    capability_.supports_penetration_depth = true;
    capability_.thread_safety =
        CollisionEngineThreadSafetyClass::kSingleContextExclusive;
    capability_.deterministic_ordering = true;
  }

  const CollisionEngineCapabilityDescriptor& capability() const override {
    return capability_;
  }

  void set_distance_capabilities(
      std::vector<CollisionDistanceRequestMode> modes,
      bool supports_distance, bool supports_penetration) {
    capability_.supported_distance_request_modes = std::move(modes);
    capability_.supports_distance_query = supports_distance;
    capability_.supports_penetration_depth = supports_penetration;
  }

  CollisionResult check(const CollisionWorldSnapshot& snapshot,
                        const CollisionQuery& query,
                        const CollisionPolicySnapshot&,
                        const CollisionCancellationToken& cancel) override {
    ++check_count;
    observed_queries.push_back(query);
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
  auto entry = testing::release_rule(pair);
  testing::release_of(entry).max_start_penetration_m = max_start_penetration_m;
  testing::release_of(entry).release_tolerance_m = 1e-5;
  testing::release_of(entry).min_release_progress_m = 0.0;
  testing::release_of(entry).require_separation_by_end = require_separation_by_end;
  testing::release_of(entry).require_strictly_decreasing_penetration =
      require_strictly_decreasing_penetration;
  testing::release_of(entry).allow_recontact = false;
  testing::add_contact_rule(s, entry);
  return s;
}

CollisionPolicySnapshot make_binary_contact_release_snapshot(
    const CollisionPairId& pair, bool require_separation_by_end = true,
    bool allow_recontact = false) {
  auto snapshot = make_contact_release_snapshot(
      pair, /*max_start_penetration_m=*/0.0, require_separation_by_end,
      /*require_strictly_decreasing_penetration=*/false);
  auto& entry = snapshot.contact_rules.front();
  testing::release_of(entry).release_evidence_mode = ContactReleaseEvidenceMode::kBinaryContact;
  testing::release_of(entry).enforce_max_start_penetration = false;
  testing::release_of(entry).max_start_penetration_m = 0.0;
  testing::release_of(entry).release_tolerance_m = 0.0;
  testing::release_of(entry).min_release_progress_m = 0.0;
  testing::release_of(entry).require_strictly_decreasing_penetration = false;
  testing::release_of(entry).allow_recontact = allow_recontact;
  return snapshot;
}

/// One pair under a whole-move contact rule with a depth limit: the pair may
/// appear, disappear and touch at the final sample as long as every colliding
/// depth stays within the limit.
CollisionPolicySnapshot make_bounded_penetration_snapshot(
    const CollisionPairId& pair, double max_penetration_m) {
  CollisionPolicySnapshot s = make_freespace_snapshot();
  testing::add_contact_rule(s, testing::contact_rule(pair, max_penetration_m, std::nullopt, "bounded_contact"));
  return s;
}

const char kFingerLeft[] = "component/arm/model/arm_model/body/finger_left/collision/0";
const char kFingerRight[] = "component/arm/model/arm_model/body/finger_right/collision/0";
const char kPartShape[] = "scene/part/shape/0";

/// Both fingers may touch the part in the last quarter of the move, at most
/// 3 mm deep (rule `fingers_on_part`).
CollisionPolicySnapshot make_windowed_contact_snapshot(
    std::optional<double> max_penetration_m = 0.003) {
  CollisionPolicySnapshot s = make_freespace_snapshot();
  s.require_full_diagnostics = true;
  testing::add_contact_rule(s, testing::contact_rule({kFingerLeft, kPartShape}, max_penetration_m, 0.75, "fingers_on_part"));
  testing::add_contact_rule(s, testing::contact_rule({kFingerRight, kPartShape}, max_penetration_m,
                                                  0.75, "fingers_on_part"));
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
  auto make_body = [](std::string body_id) {
    CollisionWorldBody body;
    body.body_id = std::move(body_id);
    return body;
  };
  snapshot.bodies = {
      make_body("component/arm/model/arm_model/body/finger_left/collision/0"),
      make_body("component/arm/model/arm_model/body/finger_right/collision/0"),
      make_body("scene/part/shape/0"),
  };
  snapshot.body_count = static_cast<std::int64_t>(snapshot.bodies.size());
  snapshot.geometry_count = 0;
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
      make_colliding_result(CollisionPairId{"component/arm/model/arm_model/body/link/collision/0", "scene/part/shape/0"});
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
      make_colliding_result(CollisionPairId{"component/arm/model/arm_model/body/link/collision/0", "scene/part/shape/0"});
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

TEST(CollisionValidationCoordinatorTest,
     SignedReleaseRequestsOnlySelectedCollidingLogicalPairs) {
  const CollisionPairId authored_pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
  const CollisionPairId canonical_pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
  MockCollisionQueryEngine engine;
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_contact_release_snapshot(
      authored_pair, /*max_start=*/0.005,
      /*require_separation_by_end=*/false,
      /*require_strictly_decreasing_penetration=*/false);
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(1), cancel);

  EXPECT_TRUE(bundle.decision.accepted);
  ASSERT_EQ(engine.observed_queries.size(), 1u);
  const auto& query = engine.observed_queries.front();
  EXPECT_EQ(query.distance_request_mode,
            CollisionDistanceRequestMode::kSelectedCollidingPairs);
  ASSERT_EQ(query.selected_distance_pairs.size(), 1u);
  EXPECT_EQ(query.selected_distance_pairs.front(), canonical_pair);
  EXPECT_NE(std::find(query.required_capabilities.begin(),
                      query.required_capabilities.end(),
                      "penetration_depth"),
            query.required_capabilities.end());
  EXPECT_EQ(std::find(query.required_capabilities.begin(),
                      query.required_capabilities.end(), "distance"),
            query.required_capabilities.end());
}

TEST(CollisionValidationCoordinatorTest,
     BinaryReleaseRequestsNoDistanceEvidence) {
  const CollisionPairId pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
  MockCollisionQueryEngine engine;
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_binary_contact_release_snapshot(
      pair, /*require_separation_by_end=*/false);
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(1), cancel);

  EXPECT_TRUE(bundle.decision.accepted);
  ASSERT_EQ(engine.observed_queries.size(), 1u);
  const auto& query = engine.observed_queries.front();
  EXPECT_EQ(query.distance_request_mode, CollisionDistanceRequestMode::kNone);
  EXPECT_TRUE(query.selected_distance_pairs.empty());
  EXPECT_EQ(std::find(query.required_capabilities.begin(),
                      query.required_capabilities.end(),
                      "penetration_depth"),
            query.required_capabilities.end());
}

TEST(CollisionValidationCoordinatorTest,
     SelectedScopeAdmissionNeverFallsBackToAllActiveDistance) {
  const CollisionPairId pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
  MockCollisionQueryEngine engine;
  engine.set_distance_capabilities(
      {CollisionDistanceRequestMode::kNone,
       CollisionDistanceRequestMode::kAllActivePairs},
      /*supports_distance=*/true, /*supports_penetration=*/true);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_contact_release_snapshot(
      pair, /*max_start_penetration_m=*/0.005,
      /*require_separation_by_end=*/false,
      /*require_strictly_decreasing_penetration=*/false);
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(1), cancel);

  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action,
            CollisionDecisionAction::kInconclusive);
  EXPECT_EQ(engine.check_count, 0);
  ASSERT_EQ(bundle.waypoint_records.size(), 1u);
  const auto& result = bundle.waypoint_records.front().result;
  EXPECT_EQ(result.engine_status,
            CollisionEngineStatus::kDiagnosticIncomplete);
  EXPECT_EQ(result.distance_request_mode,
            CollisionDistanceRequestMode::kSelectedCollidingPairs);
  EXPECT_EQ(result.raw_diagnostics.at("reason"),
            "collision_distance_request_mode_unsupported");
  EXPECT_EQ(result.raw_diagnostics.at("requested_distance_request_mode"),
            "selected_colliding_pairs");
}

TEST(CollisionValidationCoordinatorTest,
     SelectedScopeReportsMissingPenetrationCapabilityPrecisely) {
  const CollisionPairId pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
  MockCollisionQueryEngine engine;
  engine.set_distance_capabilities(
      {CollisionDistanceRequestMode::kNone,
       CollisionDistanceRequestMode::kSelectedCollidingPairs},
      /*supports_distance=*/true, /*supports_penetration=*/false);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_contact_release_snapshot(
      pair, /*max_start_penetration_m=*/0.005,
      /*require_separation_by_end=*/false,
      /*require_strictly_decreasing_penetration=*/false);
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(1), cancel);

  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action,
            CollisionDecisionAction::kInconclusive);
  EXPECT_EQ(engine.check_count, 0);
  ASSERT_EQ(bundle.waypoint_records.size(), 1u);
  const auto& result = bundle.waypoint_records.front().result;
  EXPECT_EQ(result.engine_status,
            CollisionEngineStatus::kDiagnosticIncomplete);
  EXPECT_EQ(result.distance_request_mode,
            CollisionDistanceRequestMode::kSelectedCollidingPairs);
  EXPECT_EQ(result.raw_diagnostics.at("reason"),
            "collision_required_capability_unsupported");
  EXPECT_EQ(result.raw_diagnostics.at("required_capability"),
            "penetration_depth");
  EXPECT_EQ(result.raw_diagnostics.at("requested_distance_request_mode"),
            "selected_colliding_pairs");
  std::map<std::string, std::string> details;
  append_collision_failure_diagnostics(details, bundle);
  EXPECT_EQ(details.at("collision_result_raw_required_capability"),
            "penetration_depth");
}

TEST(CollisionValidationCoordinatorTest,
     BinaryContactAdmitsCollisionOnlyEngineWithExplicitNoneScope) {
  const CollisionPairId pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
  MockCollisionQueryEngine engine;
  engine.set_distance_capabilities(
      {CollisionDistanceRequestMode::kNone},
      /*supports_distance=*/false, /*supports_penetration=*/false);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_binary_contact_release_snapshot(
      pair, /*require_separation_by_end=*/false);
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(1), cancel);

  EXPECT_TRUE(bundle.decision.accepted);
  EXPECT_EQ(engine.check_count, 1);
  ASSERT_EQ(engine.observed_queries.size(), 1u);
  EXPECT_EQ(engine.observed_queries.front().distance_request_mode,
            CollisionDistanceRequestMode::kNone);
}

TEST(CollisionValidationCoordinatorTest,
     BoundedPenetrationRequestsOnlyItsSelectedCollidingPair) {
  const CollisionPairId authored_pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
  const CollisionPairId canonical_pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
  MockCollisionQueryEngine engine;
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot =
      make_bounded_penetration_snapshot(authored_pair, 0.005);
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(1), cancel);

  EXPECT_TRUE(bundle.decision.accepted);
  ASSERT_EQ(engine.observed_queries.size(), 1u);
  const auto& query = engine.observed_queries.front();
  EXPECT_EQ(query.distance_request_mode,
            CollisionDistanceRequestMode::kSelectedCollidingPairs);
  ASSERT_EQ(query.selected_distance_pairs.size(), 1u);
  EXPECT_EQ(query.selected_distance_pairs.front(), canonical_pair);
  EXPECT_NE(std::find(query.required_capabilities.begin(),
                      query.required_capabilities.end(),
                      "penetration_depth"),
            query.required_capabilities.end());
  EXPECT_EQ(std::find(query.required_capabilities.begin(),
                      query.required_capabilities.end(), "distance"),
            query.required_capabilities.end());
}

TEST(CollisionValidationCoordinatorTest,
     MixedReleasePolicySelectsOnlySignedPairs) {
  const CollisionPairId signed_pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
  const CollisionPairId binary_pair{"scene/part/shape/0", "scene/table/shape/0"};
  MockCollisionQueryEngine engine;
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_contact_release_snapshot(
      signed_pair, /*max_start=*/0.005,
      /*require_separation_by_end=*/false,
      /*require_strictly_decreasing_penetration=*/false);
  const auto binary_snapshot = make_binary_contact_release_snapshot(
      binary_pair, /*require_separation_by_end=*/false);
  testing::add_contact_rule(cfg.policy_snapshot, binary_snapshot.contact_rules.front());
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(1), cancel);

  EXPECT_TRUE(bundle.decision.accepted);
  ASSERT_EQ(engine.observed_queries.size(), 1u);
  const auto& query = engine.observed_queries.front();
  EXPECT_EQ(query.distance_request_mode,
            CollisionDistanceRequestMode::kSelectedCollidingPairs);
  ASSERT_EQ(query.selected_distance_pairs.size(), 1u);
  EXPECT_EQ(query.selected_distance_pairs.front(), signed_pair);
}

TEST(CollisionValidationCoordinatorTest,
     MixedSignedBinaryAndBoundedContactsPreserveIndependentContracts) {
  const CollisionPairId signed_pair{"component/arm/model/arm_model/body/tool/collision/0", "scene/signed/shape/0"};
  const CollisionPairId binary_pair{"scene/binary/shape/0", "scene/table/shape/0"};
  const CollisionPairId bounded_pair{"scene/insert/shape/0", "scene/socket/shape/0"};

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_contact_release_snapshot(
      signed_pair, /*max_start_penetration_m=*/0.005,
      /*require_separation_by_end=*/true,
      /*require_strictly_decreasing_penetration=*/true);
  const auto binary = make_binary_contact_release_snapshot(
      binary_pair, /*require_separation_by_end=*/true);
  const auto bounded =
      make_bounded_penetration_snapshot(bounded_pair, 0.005);
  testing::add_contact_rule(cfg.policy_snapshot, binary.contact_rules.front());
  testing::add_contact_rule(cfg.policy_snapshot, bounded.contact_rules.front());

  auto sample = [](std::vector<CollisionPairId> colliding,
                   std::vector<CollisionPairClearance> clearances) {
    std::sort(colliding.begin(), colliding.end());
    std::sort(clearances.begin(), clearances.end(),
              [](const auto& lhs, const auto& rhs) {
                return lhs.pair < rhs.pair;
              });
    CollisionResult result;
    result.in_collision = !colliding.empty();
    result.normalized_pairs = std::move(colliding);
    result.pair_clearances = std::move(clearances);
    result.engine_status = CollisionEngineStatus::kOk;
    result.decision = result.in_collision ? CollisionDecisionResult::kFail
                                          : CollisionDecisionResult::kPass;
    return result;
  };

  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[0] = sample(
      {signed_pair, binary_pair}, {{signed_pair, -0.004}});
  engine.result_by_waypoint[1] = sample(
      {signed_pair, binary_pair}, {{signed_pair, -0.002}});
  engine.result_by_waypoint[2] = sample(
      {binary_pair, bounded_pair}, {{bounded_pair, -0.003}});
  engine.result_by_waypoint[3] = sample(
      {bounded_pair}, {{bounded_pair, -0.004}});

  CollisionValidationCoordinator coordinator(cfg);
  CollisionCancellationToken cancel;
  const auto bundle = coordinator.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(4), cancel);

  ASSERT_TRUE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kDispatch);
  ASSERT_EQ(bundle.waypoint_records.size(), 4u);
  ASSERT_EQ(engine.observed_queries.size(), 4u);

  std::vector<CollisionPairId> expected_selected{
      bounded_pair, signed_pair};
  for (auto& pair : expected_selected) {
    if (pair.body_b < pair.body_a) {
      std::swap(pair.body_a, pair.body_b);
    }
  }
  std::sort(expected_selected.begin(), expected_selected.end());
  for (const auto& query : engine.observed_queries) {
    EXPECT_EQ(query.distance_request_mode,
              CollisionDistanceRequestMode::kSelectedCollidingPairs);
    EXPECT_EQ(query.selected_distance_pairs, expected_selected);
    EXPECT_EQ(std::find(query.selected_distance_pairs.begin(),
                        query.selected_distance_pairs.end(), binary_pair),
              query.selected_distance_pairs.end());
  }

  for (const auto& record : bundle.waypoint_records) {
    EXPECT_EQ(record.result.distance_request_mode,
              CollisionDistanceRequestMode::kSelectedCollidingPairs);
    EXPECT_EQ(std::find_if(record.result.pair_clearances.begin(),
                           record.result.pair_clearances.end(),
                           [&](const auto& clearance) {
                             return clearance.pair == binary_pair;
                           }),
              record.result.pair_clearances.end());
  }
  EXPECT_TRUE(bundle.waypoint_records[3].result.in_collision);
  EXPECT_NE(std::find(bundle.waypoint_records[3].result.normalized_pairs.begin(),
                      bundle.waypoint_records[3].result.normalized_pairs.end(),
                      bounded_pair),
            bundle.waypoint_records[3].result.normalized_pairs.end());
  EXPECT_TRUE(bundle.waypoint_records[3].result.pair_clearances.size() == 1u);
  EXPECT_EQ(bundle.waypoint_records[3].result.pair_clearances.front().pair,
            bounded_pair);
}

TEST(CollisionValidationCoordinatorTest,
     WindowedContactRequestsOnlyItsPairsThatNeedADepth) {
  MockCollisionQueryEngine engine;
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_windowed_contact_snapshot();
  const auto binary = make_binary_contact_release_snapshot(
      {"scene/table/shape/0", "scene/binary_part/shape/0"},
      /*require_separation_by_end=*/false);
  testing::add_contact_rule(cfg.policy_snapshot, binary.contact_rules.front());
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(1), cancel);

  EXPECT_TRUE(bundle.decision.accepted);
  ASSERT_EQ(engine.observed_queries.size(), 1u);
  const auto& query = engine.observed_queries.front();
  EXPECT_EQ(query.distance_request_mode,
            CollisionDistanceRequestMode::kSelectedCollidingPairs);
  const std::vector<CollisionPairId> expected{
      {"component/arm/model/arm_model/body/finger_left/collision/0", "scene/part/shape/0"},
      {"component/arm/model/arm_model/body/finger_right/collision/0", "scene/part/shape/0"},
  };
  EXPECT_EQ(query.selected_distance_pairs, expected);
  EXPECT_NE(std::find(query.required_capabilities.begin(),
                      query.required_capabilities.end(),
                      "penetration_depth"),
            query.required_capabilities.end());
}

// --- Contact-release on the engine-backed mock -----------------

TEST(CollisionValidationCoordinatorTest,
     SignedContactReleaseAllowsAbsentPairWithoutNumericClearance) {
  CollisionPairId pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
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

  EXPECT_TRUE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kDispatch);
  EXPECT_FALSE(bundle.decision.first_failing_waypoint.has_value());
}

TEST(CollisionValidationCoordinatorTest,
     SignedContactReleaseFailsClosedWhenCollidingPairMetricIsMissing) {
  CollisionPairId pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
  MockCollisionQueryEngine engine;
  engine.default_result = make_colliding_result(pair);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_contact_release_snapshot(
      pair, /*max_start=*/0.005,
      /*require_separation_by_end=*/false,
      /*require_strictly_decreasing_penetration=*/false);
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(3), cancel);

  ASSERT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint);
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 0);
  EXPECT_EQ(bundle.waypoint_records[0]
                .result.raw_diagnostics.at("contact_release_violation"),
            "missing_contact_release_clearance");
}

TEST(CollisionValidationCoordinatorTest,
     BoundedPenetrationAllowsAppearanceRecontactAndFinalContactUnderCap) {
  const CollisionPairId pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[0] = make_pass_result_without_clearance();
  engine.result_by_waypoint[1] =
      make_colliding_result_with_clearance(pair, -0.004);
  engine.result_by_waypoint[2] = make_pass_result_without_clearance();
  engine.result_by_waypoint[3] =
      make_colliding_result_with_clearance(pair, -0.003);
  engine.result_by_waypoint[4] =
      make_colliding_result_with_clearance(pair, -0.005);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_bounded_penetration_snapshot(pair, 0.005);
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(5), cancel);

  EXPECT_TRUE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kDispatch);
  EXPECT_FALSE(bundle.decision.first_failing_waypoint);
}

TEST(CollisionValidationCoordinatorTest,
     BoundedPenetrationRejectsFirstOverCapCollidingSample) {
  const CollisionPairId pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
  MockCollisionQueryEngine engine;
  // Bounded penetration does not require baseline contact. The first contact
  // may appear later; it is rejected only because this sample exceeds the cap.
  engine.result_by_waypoint[0] = make_pass_result_without_clearance();
  engine.result_by_waypoint[1] = make_pass_result_without_clearance();
  engine.result_by_waypoint[2] =
      make_colliding_result_with_clearance(pair, -0.006);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_bounded_penetration_snapshot(pair, 0.005);
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(3), cancel);

  ASSERT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kReject);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint);
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 2);
  const auto& diagnostics =
      bundle.waypoint_records[2].result.raw_diagnostics;
  EXPECT_EQ(diagnostics.at("contact_release_violation"),
            "contact_penetration_exceeded");
  EXPECT_EQ(diagnostics.at("contact_release_mode"), "contact");
  EXPECT_EQ(diagnostics.at("contact_release_rule_id"), "bounded_contact");
  EXPECT_EQ(diagnostics.at("contact_release_max_penetration_m"),
            "0.005000");
  EXPECT_EQ(diagnostics.at("contact_release_current_penetration_m"),
            "0.006000");
  const auto violation = contact_rule_violation_details(
      bundle, cfg.policy_snapshot, "block_dispatch");
  ASSERT_TRUE(violation.has_value());
  EXPECT_EQ(violation->at("contact_rule_violation"), "contact_penetration_exceeded");
  EXPECT_EQ(violation->at("policy_mode"), "contact");
  EXPECT_EQ(violation->at("collision_rule_id"), "bounded_contact");
  EXPECT_EQ(violation->at("contact_body_a"),
            "component/arm/model/arm_model/body/grasp/collision/0");
  EXPECT_EQ(violation->at("contact_body_b"), "scene/box/shape/0");
  EXPECT_EQ(violation->find("zone_id"), violation->end());
  EXPECT_EQ(violation->at("contact_present"), "true");
  EXPECT_EQ(violation->at("initial_contact_pair_count"), "0");
  EXPECT_EQ(violation->at("current_contact_pair_count"), "1");
  EXPECT_EQ(violation->at("distance_request_mode"),
            "selected_colliding_pairs");
  EXPECT_EQ(violation->at("evaluated_distance_pair_count"), "1");
  EXPECT_DOUBLE_EQ(std::stod(violation->at("penetration_depth_m")), 0.006);
  EXPECT_DOUBLE_EQ(std::stod(violation->at("max_penetration_m")), 0.005);
  EXPECT_EQ(violation->at("within_penetration_bound"), "false");
  EXPECT_EQ(violation->at("response"), "block_dispatch");
  EXPECT_EQ(violation->at("classification"), "blocking_collision");
  EXPECT_EQ(violation->find("release_evidence_mode"), violation->end());
}

TEST(CollisionValidationCoordinatorTest,
     BoundedPenetrationFailsClosedWhenCollidingMetricIsMissing) {
  const CollisionPairId pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[0] = make_pass_result_without_clearance();
  engine.result_by_waypoint[1] = make_colliding_result(pair);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_bounded_penetration_snapshot(pair, 0.005);
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(2), cancel);

  ASSERT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint);
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 1);
  EXPECT_EQ(bundle.waypoint_records[1]
                .result.raw_diagnostics.at("contact_release_violation"),
            "missing_contact_release_clearance");
}

TEST(CollisionValidationCoordinatorTest,
     BoundedPenetrationDoesNotAllowUndeclaredCollisionPairs) {
  const CollisionPairId configured_pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
  const CollisionPairId undeclared_pair{"component/arm/model/arm_model/body/elbow/collision/0", "scene/table/shape/0"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[0] = make_colliding_result(undeclared_pair);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot =
      make_bounded_penetration_snapshot(configured_pair, 0.005);
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(1), cancel);

  ASSERT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kReject);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint);
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 0);
  ASSERT_EQ(bundle.waypoint_records.front()
                .result.normalized_pairs_classified.size(),
            1u);
  EXPECT_EQ(bundle.waypoint_records.front()
                .result.normalized_pairs_classified.front()
                .classification,
            CollisionPairClassification::kBlockingCollision);
}

TEST(CollisionValidationCoordinatorTest,
     BinaryContactAllowsPlateausAndSeparationWithoutMetrics) {
  const CollisionPairId pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[0] = make_colliding_result(pair);
  engine.result_by_waypoint[1] = make_colliding_result(pair);
  engine.result_by_waypoint[2] = make_pass_result_without_clearance();
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_binary_contact_release_snapshot(pair);
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(3), cancel);

  EXPECT_TRUE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kDispatch);
}

TEST(CollisionValidationCoordinatorTest,
     BinaryContactRejectsPairThatWasInitiallyClear) {
  const CollisionPairId pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[1] = make_colliding_result(pair);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_binary_contact_release_snapshot(
      pair, /*require_separation_by_end=*/false);
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(2), cancel);

  ASSERT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kReject);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint);
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 1);
  EXPECT_EQ(bundle.waypoint_records[1]
                .result.raw_diagnostics.at("contact_release_violation"),
            "new_active_contact_after_clear_start");
}

TEST(CollisionValidationCoordinatorTest,
     BinaryContactRequiresCurrentFinalAbsence) {
  const CollisionPairId pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
  MockCollisionQueryEngine engine;
  engine.default_result = make_colliding_result(pair);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_binary_contact_release_snapshot(pair);
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(2), cancel);

  ASSERT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kReject);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint);
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 1);
  const auto& diagnostics =
      bundle.waypoint_records.back().result.raw_diagnostics;
  EXPECT_EQ(diagnostics.at("contact_release_violation"),
            "final_sample_not_separated");
  EXPECT_EQ(diagnostics.at("contact_release_evidence_mode"),
            "binary_contact");
  EXPECT_EQ(diagnostics.find("contact_release_current_penetration_m"),
            diagnostics.end());
  const auto violation = contact_rule_violation_details(
      bundle, cfg.policy_snapshot, "block_dispatch");
  ASSERT_TRUE(violation.has_value());
  EXPECT_EQ(violation->at("release_evidence_mode"), "binary_contact");
  EXPECT_EQ(violation->at("distance_request_mode"), "none");
  EXPECT_EQ(violation->at("evaluated_distance_pair_count"), "0");
  EXPECT_EQ(violation->find("penetration_depth_m"), violation->end());
  EXPECT_EQ(violation->at("policy_mode"), "release");
  EXPECT_EQ(violation->at("collision_rule_id"), "release_rule");
  EXPECT_EQ(violation->find("max_penetration_m"), violation->end());
}

TEST(CollisionValidationCoordinatorTest,
     ContactReleaseReportsRecontactAfterSparseSeparatedSample) {
  CollisionPairId pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[0] = make_pass_result_with_clearance(pair, -0.003);
  engine.result_by_waypoint[1] = make_pass_result_without_clearance();
  engine.result_by_waypoint[2] = make_pass_result_with_clearance(pair, -0.001);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot =
      make_contact_release_snapshot(pair, /*max_start=*/0.005,
                                    /*require_separation_by_end=*/false,
                                    /*require_strictly_decreasing_penetration=*/false);
  CollisionValidationCoordinator coord(cfg);

  const auto bundle =
      coord.validate_trajectory(make_snapshot(), engine, make_trivial_trajectory(3), cancel);

  ASSERT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kReject);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint);
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 2);
  EXPECT_EQ(bundle.waypoint_records[2]
                .result.raw_diagnostics.at("contact_release_violation"),
            "recontact_after_release");
}

TEST(CollisionValidationCoordinatorTest,
     ContactReleaseStrictlyDecreasingPenetrationPasses) {
  // Initial contact with penetration <= max_start, depth monotonically
  // decreases to 0 (clearance crosses positive at the end). Must pass.
  CollisionPairId pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[0] = make_pass_result_with_clearance(pair, -0.003);
  engine.result_by_waypoint[1] = make_pass_result_with_clearance(pair, -0.001);
  engine.result_by_waypoint[2] = make_pass_result_without_clearance();
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
  CollisionPairId pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[0] = make_pass_result_with_clearance(pair, -0.020);
  engine.result_by_waypoint[1] = make_pass_result_with_clearance(pair, -0.010);
  engine.result_by_waypoint[2] = make_pass_result_without_clearance();
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
  std::map<std::string, std::string> details;
  append_collision_failure_diagnostics(details, bundle);
  EXPECT_EQ(pre_dispatch_collision_validation_message(
                details, bundle.decision.action),
            "Release pair starts deeper than its rule allows: "
            "component/arm/model/arm_model/body/grasp/collision/0 "
            "<-> scene/box/shape/0");
}

TEST(CollisionValidationCoordinatorTest,
     ContactReleaseInitialPenetrationEnvelopeCanBeDisabled) {
  CollisionPairId pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
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
  testing::release_of(cfg.policy_snapshot.contact_rules.front())
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
  CollisionPairId pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
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
  CollisionPairId pair{"component/arm/model/arm_model/body/grasp/collision/0", "scene/box/shape/0"};
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

  std::map<std::string, std::string> details;
  append_collision_failure_diagnostics(details, bundle);
  EXPECT_EQ(details.at("collision_result_contact_release_violation"),
            "final_sample_not_separated");
  EXPECT_EQ(details.at("collision_result_contact_release_pair"),
            "component/arm/model/arm_model/body/grasp/collision/0 <-> "
            "scene/box/shape/0");
  EXPECT_EQ(pre_dispatch_collision_validation_message(
                details, bundle.decision.action),
            "Release pair did not separate by the final waypoint: "
            "component/arm/model/arm_model/body/grasp/collision/0 <-> "
            "scene/box/shape/0");
}

TEST(CollisionValidationCoordinatorTest,
     WindowedContactAllowsContactInItsWindow) {
  CollisionPairId pair{"component/arm/model/arm_model/body/finger_left/collision/0", "scene/part/shape/0"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[3] =
      make_colliding_result_with_clearance(pair, -0.001);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_windowed_contact_snapshot();
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(4), cancel);

  EXPECT_TRUE(bundle.decision.accepted);
  ASSERT_EQ(bundle.waypoint_records[3].result.normalized_pairs_classified.size(),
            1u);
  EXPECT_EQ(bundle.waypoint_records[3]
                .result.normalized_pairs_classified.front()
                .classification,
            CollisionPairClassification::kAllowedContactRule);
}

TEST(CollisionValidationCoordinatorTest,
     WindowedContactRejectsCollidingPairWithPositiveClearance) {
  const CollisionPairId pair{
      "component/arm/model/arm_model/body/finger_left/collision/0",
      "scene/part/shape/0"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[3] =
      make_colliding_result_with_clearance(pair, 0.001);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_windowed_contact_snapshot();
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(4), cancel);

  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  EXPECT_EQ(bundle.waypoint_records[3].result.engine_status,
            CollisionEngineStatus::kDiagnosticIncomplete);
  EXPECT_EQ(bundle.waypoint_records[3]
                .result.raw_diagnostics.at("contact_rule_violation"),
            "missing_signed_clearance");
}

TEST(CollisionValidationCoordinatorTest,
     WindowedContactFailsDiagnosticIncompleteWhenCollidingClearanceIsAbsent) {
  const CollisionPairId pair{"component/arm/model/arm_model/body/finger_left/collision/0", "scene/part/shape/0"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[3] = make_colliding_result(pair);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_windowed_contact_snapshot();
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(4), cancel);

  ASSERT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  ASSERT_EQ(bundle.waypoint_records.size(), 4u);
  const auto& record = bundle.waypoint_records[3];
  EXPECT_TRUE(record.result.pair_clearances.empty());
  EXPECT_EQ(record.result.engine_status,
            CollisionEngineStatus::kDiagnosticIncomplete);
  EXPECT_EQ(record.result.decision,
            CollisionDecisionResult::kInconclusiveFail);
  EXPECT_EQ(record.result.raw_diagnostics.at("contact_rule_violation"),
            "missing_signed_clearance");
}

TEST(CollisionValidationCoordinatorTest,
     WindowedContactEnrichesEngineDiagnosticIncompleteForMatchedMissingMetric) {
  const CollisionPairId pair{"component/arm/model/arm_model/body/finger_left/collision/0", "scene/part/shape/0"};
  MockCollisionQueryEngine engine;
  auto incomplete = make_colliding_result(pair);
  incomplete.engine_status = CollisionEngineStatus::kDiagnosticIncomplete;
  incomplete.decision = CollisionDecisionResult::kInconclusiveFail;
  incomplete.raw_diagnostics["missing_selected_distance_pairs"] =
      "component/arm/model/arm_model/body/finger_left/collision/0 <-> "
      "scene/part/shape/0";
  engine.result_by_waypoint[3] = std::move(incomplete);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_windowed_contact_snapshot();
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(4), cancel);

  ASSERT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 3);
  const auto& record = bundle.waypoint_records[3];
  ASSERT_EQ(record.result.normalized_pairs_classified.size(), 1u);
  EXPECT_EQ(record.result.normalized_pairs_classified.front().classification,
            CollisionPairClassification::kDiagnosticIncomplete);
  EXPECT_EQ(record.result.raw_diagnostics.at("contact_rule_violation"),
            "missing_signed_clearance");

  const auto violation = contact_rule_violation_details(
      bundle, cfg.policy_snapshot, "block_dispatch");
  ASSERT_TRUE(violation.has_value());
  EXPECT_EQ(violation->at("contact_rule_violation"),
            "missing_grasp_contact_clearance");
  EXPECT_EQ(violation->at("collision_rule_id"), "fingers_on_part");
  EXPECT_EQ(violation->at("policy_mode"), "contact");
  EXPECT_EQ(violation->at("contact_body_a"),
            "component/arm/model/arm_model/body/finger_left/collision/0");
  EXPECT_EQ(violation->at("contact_body_b"), "scene/part/shape/0");
  EXPECT_EQ(violation->at("contact_present"), "true");
  EXPECT_EQ(violation->at("distance_request_mode"),
            "selected_colliding_pairs");
  EXPECT_EQ(violation->at("evaluated_distance_pair_count"), "0");
  EXPECT_EQ(violation->at("classification"), "diagnostic_incomplete");
  EXPECT_EQ(violation->at("response"), "block_dispatch");
  EXPECT_EQ(violation->find("penetration_depth_m"), violation->end());
  EXPECT_EQ(violation->find("current_penetration_m"), violation->end());
  EXPECT_EQ(violation->find("initial_contact_pair_count"), violation->end());
  EXPECT_EQ(violation->find("current_contact_pair_count"), violation->end());
  EXPECT_EQ(violation->find("release_evidence_mode"), violation->end());
}

TEST(CollisionValidationCoordinatorTest, SpringContactRefusesACompressionJump) {
  // OD-356: whole-move spring (stroke 10 mm, 2 mm per sample) and the same
  // spring under an end window.
  const CollisionPairId pair{"component/arm/model/arm_model/body/grasp/collision/0",
                             "scene/box/shape/0"};
  for (const bool windowed : {false, true}) {
    SCOPED_TRACE(windowed);
    MockCollisionQueryEngine engine;
    for (int i = 0; i < 6; ++i) engine.result_by_waypoint[i] = make_pass_result_without_clearance();
    engine.result_by_waypoint[6] = make_colliding_result_with_clearance(pair, -0.001);
    engine.result_by_waypoint[7] = make_colliding_result_with_clearance(pair, -0.0035);
    engine.result_by_waypoint[8] = make_colliding_result_with_clearance(pair, -0.004);
    CollisionCancellationToken cancel;

    CollisionValidationPassConfig cfg;
    cfg.policy_snapshot = make_freespace_snapshot();
    cfg.policy_snapshot.require_full_diagnostics = true;
    auto rule = testing::contact_rule(pair, 0.010, windowed ? std::optional<double>(0.75)
                                                            : std::nullopt,
                                      "holder_spring");
    testing::contact_of(rule).max_compression_step_m = 0.002;
    testing::add_contact_rule(cfg.policy_snapshot, rule);
    CollisionValidationCoordinator coord(cfg);

    const auto bundle =
        coord.validate_trajectory(make_snapshot(), engine, make_trivial_trajectory(9), cancel);
    ASSERT_FALSE(bundle.decision.accepted);
    ASSERT_TRUE(bundle.decision.first_failing_waypoint);
    EXPECT_EQ(*bundle.decision.first_failing_waypoint, 7);
    const auto violation =
        contact_rule_violation_details(bundle, cfg.policy_snapshot, "block_dispatch");
    ASSERT_TRUE(violation.has_value());
    EXPECT_EQ(violation->at("contact_rule_violation"), "contact_compression_step_exceeded");
    EXPECT_EQ(violation->at("collision_rule_id"), "holder_spring");
    EXPECT_DOUBLE_EQ(std::stod(violation->at("max_compression_step_m")), 0.002);

    // The same press, 1.5 mm per sample, is accepted.
    engine.result_by_waypoint[7] = make_colliding_result_with_clearance(pair, -0.0025);
    const auto smooth =
        coord.validate_trajectory(make_snapshot(), engine, make_trivial_trajectory(9), cancel);
    EXPECT_TRUE(smooth.decision.accepted);
  }
}

TEST(CollisionValidationCoordinatorTest, GroupedSpringIsCheckedAsOneSpring) {
  // OD-371: a spring slider and the tube it carries against an endplate,
  // one rule (stroke 10 mm, 2 mm per sample), whole move and end window.
  const CollisionPairId slider{"component/holder/model/holder_model/body/holder_rod/collision/0",
                               "scene/endplate/shape/0"};
  const CollisionPairId tube{"scene/endplate/shape/0", "scene/tube/shape/0"};
  const auto sample = [&](double slider_depth, double tube_depth) {
    auto result = make_pass_result_without_clearance();
    for (const auto& [pair, depth] : {std::pair{slider, slider_depth}, std::pair{tube, tube_depth}}) {
      if (depth > 0.0) {
        result.in_collision = true;
        result.decision = CollisionDecisionResult::kFail;
        result.normalized_pairs.push_back(pair);
        result.pair_clearances.push_back(CollisionPairClearance{pair, -depth});
      }
    }
    return result;
  };
  for (const bool windowed : {false, true}) {
    for (const bool grouped : {true, false}) {
      SCOPED_TRACE(std::string(windowed ? "windowed" : "whole move") +
                   (grouped ? " grouped" : " per pair"));
      CollisionValidationPassConfig cfg;
      cfg.policy_snapshot = make_freespace_snapshot();
      cfg.policy_snapshot.require_full_diagnostics = true;
      for (const auto& pair : {slider, tube}) {
        auto rule = testing::contact_rule(pair, 0.010,
                                          windowed ? std::optional<double>(0.75) : std::nullopt,
                                          "tube_into_endplate");
        testing::contact_of(rule).max_compression_step_m = 0.002;
        if (grouped) {
          testing::contact_of(rule).spring_group = CollisionSpringGroup{
              ControllableComponentId("holder"), "holder_rod", {0.0, 0.0, 1.0}};
        }
        testing::add_contact_rule(cfg.policy_snapshot, rule);
      }
      CollisionValidationCoordinator coord(cfg);
      CollisionCancellationToken cancel;
      MockCollisionQueryEngine engine;
      for (int i = 0; i < 6; ++i) engine.result_by_waypoint[i] = make_pass_result_without_clearance();
      // The contact moves from the tube to the slider: the rule's depth goes
      // 1 mm -> 2.6 mm -> 4 mm, the slider's alone 0 -> 2.6 mm.
      engine.result_by_waypoint[6] = sample(0.0, 0.001);
      engine.result_by_waypoint[7] = sample(0.0026, 0.0025);
      engine.result_by_waypoint[8] = sample(0.004, 0.0);

      const auto handover =
          coord.validate_trajectory(make_snapshot(), engine, make_trivial_trajectory(9), cancel);
      if (grouped) {
        EXPECT_TRUE(handover.decision.accepted);
      } else {
        ASSERT_FALSE(handover.decision.accepted);
        ASSERT_TRUE(handover.decision.first_failing_waypoint);
        EXPECT_EQ(*handover.decision.first_failing_waypoint, 7);
        const auto violation =
            contact_rule_violation_details(handover, cfg.policy_snapshot, "block_dispatch");
        ASSERT_TRUE(violation.has_value());
        EXPECT_EQ(violation->at("contact_rule_violation"), "contact_compression_step_exceeded");
        continue;
      }

      // The rule's depth jumps 2.6 mm -> 5.5 mm: refused on the deepest pair.
      engine.result_by_waypoint[8] = sample(0.0055, 0.003);
      const auto jump =
          coord.validate_trajectory(make_snapshot(), engine, make_trivial_trajectory(9), cancel);
      ASSERT_FALSE(jump.decision.accepted);
      ASSERT_TRUE(jump.decision.first_failing_waypoint);
      EXPECT_EQ(*jump.decision.first_failing_waypoint, 8);
      const auto violation =
          contact_rule_violation_details(jump, cfg.policy_snapshot, "block_dispatch");
      ASSERT_TRUE(violation.has_value());
      EXPECT_EQ(violation->at("contact_rule_violation"), "contact_compression_step_exceeded");
      EXPECT_EQ(violation->at("collision_rule_id"), "tube_into_endplate");
      EXPECT_EQ(violation->at("contact_body_a"), slider.body_a);
    }
  }
}

TEST(CollisionValidationCoordinatorTest,
     WindowedContactOverLimitProducesViolationMetricEvidence) {
  const CollisionPairId pair{"component/arm/model/arm_model/body/finger_left/collision/0", "scene/part/shape/0"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[3] =
      make_colliding_result_with_clearance(pair, -0.004);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_windowed_contact_snapshot();
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(4), cancel);

  ASSERT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kReject);
  EXPECT_EQ(bundle.waypoint_records[3]
                .result.raw_diagnostics.at("contact_rule_violation"),
            "contact_penetration_exceeded");
  const auto violation = contact_rule_violation_details(
      bundle, cfg.policy_snapshot, "block_dispatch");
  ASSERT_TRUE(violation.has_value());
  EXPECT_EQ(violation->at("contact_rule_violation"),
            "contact_penetration_exceeded");
  EXPECT_EQ(violation->at("collision_rule_id"), "fingers_on_part");
  EXPECT_EQ(violation->at("policy_mode"), "contact");
  EXPECT_EQ(violation->at("within_penetration_bound"), "false");
  EXPECT_DOUBLE_EQ(std::stod(violation->at("end_from_fraction")), 0.75);
  EXPECT_DOUBLE_EQ(std::stod(violation->at("penetration_depth_m")), 0.004);
  EXPECT_DOUBLE_EQ(std::stod(violation->at("current_penetration_m")), 0.004);
  EXPECT_DOUBLE_EQ(std::stod(violation->at("max_penetration_m")), 0.003);
  EXPECT_EQ(violation->at("inside_window"), "true");
  EXPECT_EQ(violation->find("zone_id"), violation->end());
  EXPECT_EQ(violation->find("initial_contact_pair_count"), violation->end());
  EXPECT_EQ(violation->find("release_evidence_mode"), violation->end());
}

TEST(CollisionValidationCoordinatorTest,
     WindowedContactAtTheFirstSampleKeepsItsReason) {
  const CollisionPairId pair{"component/arm/model/arm_model/body/finger_left/collision/0", "scene/part/shape/0"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[0] =
      make_colliding_result_with_clearance(pair, -0.001);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_windowed_contact_snapshot();
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(4), cancel);

  ASSERT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.waypoint_records[0]
                .result.raw_diagnostics.at("contact_rule_violation"),
            "initial_sample_in_contact");
  const auto violation = contact_rule_violation_details(
      bundle, cfg.policy_snapshot, "block_dispatch");
  ASSERT_TRUE(violation.has_value());
  EXPECT_EQ(violation->at("contact_rule_violation"),
            "initial_sample_in_contact");
  EXPECT_EQ(violation->at("inside_window"), "false");
  EXPECT_DOUBLE_EQ(std::stod(violation->at("penetration_depth_m")), 0.001);
}

TEST(CollisionValidationCoordinatorTest,
     WindowedContactRejectsContactBeforeItsWindow) {
  CollisionPairId pair{"component/arm/model/arm_model/body/finger_left/collision/0", "scene/part/shape/0"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[1] =
      make_colliding_result_with_clearance(pair, -0.001);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_windowed_contact_snapshot();
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(4), cancel);

  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kReject);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 1);
  EXPECT_EQ(bundle.waypoint_records[1]
                .result.raw_diagnostics.at("contact_rule_violation"),
            "contact_before_terminal_window");
  const auto violation = contact_rule_violation_details(
      bundle, cfg.policy_snapshot, "block_dispatch");
  ASSERT_TRUE(violation.has_value());
  EXPECT_EQ(violation->at("contact_rule_violation"),
            "grasp_contact_before_terminal_window");
  EXPECT_EQ(violation->at("policy_mode"), "contact");
  EXPECT_EQ(violation->at("inside_window"), "false");
  EXPECT_DOUBLE_EQ(std::stod(violation->at("penetration_depth_m")), 0.001);
  EXPECT_DOUBLE_EQ(std::stod(violation->at("max_penetration_m")), 0.003);
}

TEST(CollisionValidationCoordinatorTest, WindowedContactRuntimePrefixUsesOriginalTrajectoryFraction) {
  const CollisionPairId pair{"component/arm/model/arm_model/body/finger_left/collision/0", "scene/part/shape/0"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[2] = make_colliding_result_with_clearance(pair, -0.001);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_windowed_contact_snapshot();
  cfg.original_trajectory_sample_fractions = {0.25, 0.5, 0.74};
  CollisionValidationCoordinator coord(cfg);

  const auto bundle =
      coord.validate_trajectory(make_snapshot(), engine, make_trivial_trajectory(3), cancel);

  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kReject);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 2);
  EXPECT_EQ(bundle.waypoint_records[2].result.raw_diagnostics.at("contact_rule_violation"),
            "contact_before_terminal_window");
}

TEST(CollisionValidationCoordinatorTest,
     WindowedContactRuntimeTerminalPrefixDoesNotBecomeInitialSample) {
  const CollisionPairId pair{"component/arm/model/arm_model/body/finger_left/collision/0", "scene/part/shape/0"};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[0] = make_colliding_result_with_clearance(pair, -0.001);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_windowed_contact_snapshot();
  cfg.original_trajectory_sample_fractions = {0.9};
  CollisionValidationCoordinator coord(cfg);

  const auto bundle =
      coord.validate_trajectory(make_snapshot(), engine, make_trivial_trajectory(1), cancel);

  EXPECT_TRUE(bundle.decision.accepted);
  ASSERT_EQ(bundle.waypoint_records[0].result.normalized_pairs_classified.size(), 1u);
  EXPECT_EQ(bundle.waypoint_records[0].result.normalized_pairs_classified.front().classification,
            CollisionPairClassification::kAllowedContactRule);
}

TEST(CollisionValidationCoordinatorTest, InvalidRuntimeWindowedContactFractionsFailClosed) {
  MockCollisionQueryEngine engine;
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_windowed_contact_snapshot();
  cfg.original_trajectory_sample_fractions = {0.5, 0.4};
  CollisionValidationCoordinator coord(cfg);

  const auto bundle =
      coord.validate_trajectory(make_snapshot(), engine, make_trivial_trajectory(2), cancel);

  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  EXPECT_EQ(bundle.waypoint_records.front().result.raw_diagnostics.at("contact_rule_violation"),
            "invalid_contact_rule_sample_fractions");
}

TEST(CollisionValidationCoordinatorTest,
     WindowedContactInvalidRuleFailsClosed) {
  MockCollisionQueryEngine engine;
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_windowed_contact_snapshot();
  testing::contact_of(cfg.policy_snapshot.contact_rules.front()).end_from_fraction = 1.5;
  CollisionValidationCoordinator coord(cfg);

  auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(2), cancel);

  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  EXPECT_EQ(bundle.waypoint_records.front()
                .result.raw_diagnostics.at("contact_rule_violation"),
            "invalid_collision_rule");
  EXPECT_EQ(bundle.waypoint_records.front().result.raw_diagnostics.at("contact_rule_id"),
            "fingers_on_part");
}

TEST(CollisionValidationCoordinatorTest,
     WindowedContactWithoutLimitNeedsNoDepthEvidence) {
  // No max_penetration_m: the depth is not checked and no signed distance is
  // requested; the window and the first-sample refusal still apply.
  const CollisionPairId pair{kFingerLeft, kPartShape};
  MockCollisionQueryEngine engine;
  engine.result_by_waypoint[3] = make_colliding_result(pair);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_windowed_contact_snapshot(std::nullopt);
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(4), cancel);

  EXPECT_TRUE(bundle.decision.accepted);
  ASSERT_FALSE(engine.observed_queries.empty());
  EXPECT_EQ(engine.observed_queries.front().distance_request_mode,
            CollisionDistanceRequestMode::kNone);
  EXPECT_TRUE(engine.observed_queries.front().selected_distance_pairs.empty());
  EXPECT_EQ(bundle.waypoint_records[3]
                .result.normalized_pairs_classified.front()
                .classification,
            CollisionPairClassification::kAllowedContactRule);

  MockCollisionQueryEngine early_engine;
  early_engine.result_by_waypoint[1] = make_colliding_result(pair);
  CollisionValidationCoordinator early_coord(cfg);
  const auto early = early_coord.validate_trajectory(
      make_snapshot(), early_engine, make_trivial_trajectory(4), cancel);
  ASSERT_FALSE(early.decision.accepted);
  EXPECT_EQ(early.waypoint_records[1].result.raw_diagnostics.at("contact_rule_violation"),
            "contact_before_terminal_window");
}

TEST(CollisionValidationCoordinatorTest,
     SeveralWindowedContactPairsAreActiveInOneMove) {
  // Both finger pairs touch in the window; a third body is no rule pair.
  const CollisionPairId left{kFingerLeft, kPartShape};
  const CollisionPairId right{kFingerRight, kPartShape};
  MockCollisionQueryEngine engine;
  auto both = make_colliding_result_with_clearance(left, -0.001);
  both.normalized_pairs = {left, right};
  both.pair_clearances.push_back(CollisionPairClearance{right, -0.002});
  engine.result_by_waypoint[3] = both;
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_windowed_contact_snapshot();
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(4), cancel);

  EXPECT_TRUE(bundle.decision.accepted);
  ASSERT_EQ(bundle.waypoint_records[3].result.normalized_pairs_classified.size(), 2u);
  for (const auto& classified : bundle.waypoint_records[3].result.normalized_pairs_classified) {
    EXPECT_EQ(classified.classification, CollisionPairClassification::kAllowedContactRule);
  }

  const CollisionPairId palm{"component/arm/model/arm_model/body/palm/collision/0", kPartShape};
  MockCollisionQueryEngine palm_engine;
  palm_engine.result_by_waypoint[3] = make_colliding_result_with_clearance(palm, -0.001);
  CollisionValidationCoordinator palm_coord(cfg);
  const auto refused = palm_coord.validate_trajectory(
      make_snapshot(), palm_engine, make_trivial_trajectory(4), cancel);
  EXPECT_FALSE(refused.decision.accepted);
  EXPECT_EQ(refused.decision.action, CollisionDecisionAction::kReject);
}

TEST(CollisionValidationCoordinatorTest,
     WholeMoveContactWithoutLimitIsAllowedFromTheFirstSample) {
  const CollisionPairId pair{kFingerLeft, kPartShape};
  MockCollisionQueryEngine engine;
  engine.default_result = make_colliding_result(pair);
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_freespace_snapshot();
  testing::add_contact_rule(cfg.policy_snapshot, testing::contact_rule(pair));
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(3), cancel);

  EXPECT_TRUE(bundle.decision.accepted);
  ASSERT_EQ(bundle.waypoint_records.size(), 3u);
  for (const auto& record : bundle.waypoint_records) {
    ASSERT_EQ(record.result.normalized_pairs_classified.size(), 1u);
    EXPECT_EQ(record.result.normalized_pairs_classified.front().classification,
              CollisionPairClassification::kAllowedContactRule);
  }
  EXPECT_EQ(engine.observed_queries.front().distance_request_mode,
            CollisionDistanceRequestMode::kNone);
}

TEST(CollisionValidationCoordinatorTest,
     ClearanceRefusalNamesTheRuleThatHoldsThePair) {
  // The engine reports the pair as colliding because it is closer than its
  // clearance; the refusal details name the clearance rule and its value.
  const CollisionPairId pair{"component/arm/model/arm_model/body/wrist/collision/0",
                             "scene/wall/shape/0"};
  auto margins = std::make_shared<CollisionClearanceMargins>();
  margins->rule_ids = {"everything", "wrist_near_wall"};
  margins->pairs = {CollisionPairMargin{pair, 0.005, 1U}};

  MockCollisionQueryEngine engine;
  auto near = make_colliding_result(pair);
  near.clearance_violations = {CollisionPairClearance{pair, 0.002}};
  engine.result_by_waypoint[1] = near;
  CollisionCancellationToken cancel;

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = make_freespace_snapshot();
  cfg.clearance_margins = margins;
  CollisionValidationCoordinator coord(cfg);

  const auto bundle = coord.validate_trajectory(
      make_snapshot(), engine, make_trivial_trajectory(2), cancel);
  ASSERT_FALSE(bundle.decision.accepted);
  ASSERT_FALSE(engine.observed_queries.empty());
  EXPECT_EQ(engine.observed_queries.front().clearance_margins, margins);

  std::map<std::string, std::string> details;
  append_collision_failure_diagnostics(details, bundle, margins.get());
  EXPECT_EQ(details.at("classification"), "clearance");
  EXPECT_EQ(details.at("collision_rule_id"), "wrist_near_wall");
  EXPECT_DOUBLE_EQ(std::stod(details.at("clearance_m")), 0.005);
  EXPECT_DOUBLE_EQ(std::stod(details.at("signed_clearance_m")), 0.002);

  // Without the margins the refusal keeps its classification only.
  std::map<std::string, std::string> plain;
  append_collision_failure_diagnostics(plain, bundle);
  EXPECT_EQ(plain.at("classification"), "clearance");
  EXPECT_EQ(plain.find("collision_rule_id"), plain.end());
}

TEST(CollisionValidationCoordinatorTest, RejectionMessageNamesTheCollidingPair) {
  const std::map<std::string, std::string> details{
      {"collision_result_first_failure_pair", "component/arm/body/link6 <-> scene/endplate/shape/0"}};
  EXPECT_EQ(pre_dispatch_collision_validation_message(details, CollisionDecisionAction::kReject),
            "Collision validation rejected trajectory: component/arm/body/link6 <-> "
            "scene/endplate/shape/0");
  EXPECT_EQ(pre_dispatch_collision_validation_message({}, CollisionDecisionAction::kReject),
            "Collision validation rejected trajectory");
}

}  // namespace
}  // namespace manyforge::core
