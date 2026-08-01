// Owner-neutral manyforge_collision integration and relocation acceptance map.
//
// The historical source at manyforge@08222188293df8e12f90dc72e204c21f22f92ea1
// contained 35 tests. It constructed NativeKernelBundle for scene snapshots,
// observed state, planner comparison, and runtime composition. That factory is
// intentionally gone. Nine responsibilities below remain testable directly
// through ManyForgeCollisionEngine; both the retained/adapted cases and every
// unavailable historical case are named exactly once below instead of being
// hidden in a skipped legacy body.

#include "collision_qualification_support.hpp"

#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/collision_world_snapshot.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

using manyforge::collision::ManyForgeCollisionEngine;
using manyforge::core::CollisionCancellationToken;
using manyforge::core::CollisionDecisionResult;
using manyforge::core::CollisionEngineStatus;
using manyforge::core::CollisionPolicySnapshot;
using manyforge::core::CollisionQuery;
using manyforge::core::CollisionResult;
using manyforge::core::CollisionWorldSnapshot;
using manyforge::qualification::AcceptanceDisposition;

std::filesystem::path repo_root() {
  return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}

struct ModelPaths {
  std::filesystem::path urdf;
  std::filesystem::path srdf;
};

ModelPaths ur10e_model_paths() {
  const auto model_dir = repo_root() / "manyforge_example_models" / "models" /
                         "ur10e_robotiq_2f_140";
  return {
      model_dir / "ur10e_robotiq_2f_140.urdf",
      model_dir / "ur10e_robotiq_2f_140.srdf",
  };
}

ManyForgeCollisionEngine::SharedModelPtr load_shared_model() {
  const auto paths = ur10e_model_paths();
  return ManyForgeCollisionEngine::load_shared_model(
      "ur10e_robotiq_2f_140", paths.urdf, paths.srdf, {repo_root()}, "arm");
}

std::unique_ptr<ManyForgeCollisionEngine> make_engine(
    const std::string& group = "arm") {
  const auto paths = ur10e_model_paths();
  return std::make_unique<ManyForgeCollisionEngine>(
      "ur10e_robotiq_2f_140", paths.urdf, paths.srdf,
      std::vector<std::filesystem::path>{repo_root()}, group);
}

CollisionWorldSnapshot minimal_snapshot() {
  CollisionWorldSnapshot snapshot;
  snapshot.publisher_id = "owner-neutral-collision-test";
  snapshot.revision.revision_id = 1;
  snapshot.revision.content_hash = "sha256:owner-neutral-collision-test";
  snapshot.all_bodies_have_geometry = true;
  return snapshot;
}

CollisionResult check(ManyForgeCollisionEngine& engine,
                      const CollisionWorldSnapshot& snapshot,
                      CollisionQuery query,
                      const CollisionCancellationToken* cancel = nullptr) {
  query.collision_world_revision = snapshot.revision;
  CollisionCancellationToken default_cancel;
  return engine.check(snapshot, query, CollisionPolicySnapshot{},
                      cancel == nullptr ? default_cancel : *cancel);
}

constexpr std::array<AcceptanceDisposition, 26>
    kFactoryCoupledAcceptance{{
        {"FactoryValidationAndLookaheadReuseOnePreparedRevision", "I6/N",
         "addressed validation/lookahead contexts reuse one owner-issued revision"},
        {"ConcurrentColdContextsPublishOnePreparedRevision", "I6/N",
         "concurrent consumers publish one immutable prepared revision"},
        {"DedicatedRuntimeSurfacePreparesAndTracksPublishedRevisions", "I6/N+I8/P5",
         "runtime preparation, distance modes, counters, and revision tracking"},
        {"DedicatedRuntimeSurfaceRejectsDuplicateFullConfigurationNames", "I6/N",
         "resource-addressed state rejects duplicate canonical coordinates"},
        {"ConcurrentQueriesOnDifferentInstancesComplete", "I6/N",
         "independent owner-issued query contexts make concurrent progress"},
        {"ConcurrentCollidingSnapshotQueriesDoNotRaceDiagnosticCapture", "I6/N",
         "colliding diagnostics remain isolated across concurrent contexts"},
        {"AgreesWithDiagnosticComparatorOnFreeJointStateAgainstSameModel", "I6/N",
         "new diagnostic observer parity without the removed comparator owner"},
        {"SnapshotPortAcceptsPrimitiveSceneSnapshotWithBodyPairCapability", "I6/N",
         "headless scene publishes exact primitive body/resource identities"},
        {"SnapshotPortAppliesTimeoutBudgetToFullCall", "I6/N+I8/P5",
         "owner-issued query deadline covers preparation and collision work"},
        {"ObservedRuntimeMonitorReportsSnapshotSyncSeparately", "I8/P5",
         "immediate monitor reports changed-snapshot cost separately"},
        {"ObservedRuntimeLookaheadExcludesSnapshotSyncFromPerQueryBudget", "I8/P5",
         "lookahead charges preparation outside each warm query budget"},
        {"SnapshotPortLoadsTriangleMeshSceneObject", "I6/N",
         "retained mesh identity and bytes load through the headless snapshot"},
        {"PassiveMeshSceneContactDoesNotEnterMotionEnginePairGraph", "I6/N+I8/P5",
         "passive contacts cannot acquire motion-policy authority"},
        {"IsaacMeshTableCheeseStartContactDoesNotEnterMotionEnginePairGraph", "I9/I+I8/P5",
         "Isaac observed mesh contacts preserve exact policy classification"},
        {"AttachedObjectTouchLinksHonorExplicitGripperCollisionEnvelope", "I6/N+I8/P5",
         "attachments and touch links use owner-issued resource identities"},
        {"RequiredUnsupportedDiagnosticCapabilityFailsClosed", "I6/N",
         "typed required capabilities fail closed when unavailable"},
        {"RequiredDistanceCapabilityPopulatesSignedClearance", "I6/N+I8/P5",
         "signed-clearance evidence is emitted for an explicit distance scope"},
        {"RequiredDistanceCapabilityWithoutTypedScopeFailsClosed", "I6/N",
         "distance requests require an exact typed scope"},
        {"SelectedCollidingDistancePreservesFullCollisionCoverage", "I6/N+I8/P5",
         "selected distance never narrows collision enumeration"},
        {"SelectedPenetrationIsExactAcrossIndependentWorkerHistories", "I8/P5",
         "worker history cannot alter selected penetration evidence"},
        {"UnrelatedSelectedPairEvaluatesNoDistance", "I6/N",
         "unknown selected resources perform no distance work"},
        {"SelectedCollidingDistanceRequiresANonemptySelection", "I6/N",
         "empty selected-distance scope fails closed"},
        {"AllCollidingDistanceEvaluatesOnlyCollidingGeometryPairs", "I6/N+I8/P5",
         "all-colliding mode excludes unrelated distance work"},
        {"MultipleGeometryPairsReduceToDeepestLogicalPairDistance", "I6/N+I8/P5",
         "logical-pair reduction retains the deepest complete finite evidence"},
        {"QueryProceedsEvenWhilePlannerProjectionIsHeavilyBusy", "I6/N",
         "authoritative collision queries are independent of planner projection"},
        {"CompareModeEmitsMismatchEventWhenEnginesDisagree", "I6/N+I5/H0",
         "source-owned mismatch facts replace the deleted comparator emitter"},
    }};

constexpr std::array<AcceptanceDisposition, 9> kRetainedAcceptance{{
    {"ConvenienceInstancesOwnDistinctInternalAssets", "retained/current",
     "direct engine instances own distinct model, cache, and mutable contexts"},
    {"FactoryContextsShareImmutableAssetsButIsolateRuntimePreparation",
     "adapted/current",
     "explicit contexts share immutable model assets and choose cache domains"},
    {"EngineExposesStableIdAndVersion", "retained/current",
     "the direct engine exposes stable identity, version, and capability"},
    {"MixedFiniteAndNonFiniteComponentsRemainIncomplete", "retained/current",
     "owner-neutral distance evidence remains incomplete around non-finite data"},
    {"SnapshotPortRejectsStaleRevision", "retained/current",
     "the direct engine rejects a query for a different snapshot revision"},
    {"SnapshotPortRejectsUnknownSnapshotFormatWithDiagnostic", "retained/current",
     "the direct engine rejects an unknown snapshot format with diagnostics"},
    {"SnapshotPortHonorsCancellation", "retained/current",
     "the direct engine fails closed on a pre-requested cancellation"},
    {"ZeroTimeoutFailsClosedWithoutQueryPass", "retained/current",
     "the direct engine fails closed before work when no deadline remains"},
    {"MalformedQueryFailsClosedWithoutThrowing", "retained/current",
     "the direct engine reports malformed robot state without throwing"},
}};

TEST(ManyForgeCollisionEngineTest,
     ConvenienceInstancesOwnDistinctInternalAssets) {
  const auto engine_a = make_engine();
  const auto engine_b = make_engine();
  EXPECT_NE(engine_a->internal_model_identity(),
            engine_b->internal_model_identity());
  EXPECT_NE(engine_a->internal_scene_cache_identity(),
            engine_b->internal_scene_cache_identity());
  EXPECT_NE(engine_a->capability().mutable_context_id,
            engine_b->capability().mutable_context_id);
}

TEST(ManyForgeCollisionEngineTest,
     ExplicitContextsShareImmutableAssetsAndChooseCacheDomains) {
  const auto model = load_shared_model();
  const auto shared_cache = ManyForgeCollisionEngine::make_shared_scene_cache(model);
  const auto isolated_cache = ManyForgeCollisionEngine::make_shared_scene_cache(model);
  ManyForgeCollisionEngine validation(model, shared_cache);
  ManyForgeCollisionEngine lookahead(model, shared_cache);
  ManyForgeCollisionEngine immediate(model, isolated_cache);

  EXPECT_EQ(validation.internal_model_identity(), lookahead.internal_model_identity());
  EXPECT_EQ(lookahead.internal_model_identity(), immediate.internal_model_identity());
  EXPECT_EQ(validation.internal_scene_cache_identity(),
            lookahead.internal_scene_cache_identity());
  EXPECT_NE(validation.internal_scene_cache_identity(),
            immediate.internal_scene_cache_identity());
  EXPECT_NE(validation.capability().mutable_context_id,
            lookahead.capability().mutable_context_id);
}

TEST(ManyForgeCollisionEngineTest, EngineExposesStableIdAndVersion) {
  const auto engine = make_engine();
  EXPECT_FALSE(engine->engine_id().empty());
  EXPECT_FALSE(engine->engine_version().empty());
  EXPECT_FALSE(engine->capability().mutable_context_id.empty());
}

TEST(ManyForgeCollisionDistanceEvidenceTest,
     MixedFiniteAndNonFiniteComponentsRemainIncomplete) {
  manyforge::collision::detail::SelectedCollidingDistanceEvidence evidence;
  evidence.observe(-0.001);
  evidence.observe(-0.003);
  evidence.observe(std::numeric_limits<double>::quiet_NaN());
  evidence.observe(-0.002);
  EXPECT_FALSE(evidence.complete);
  ASSERT_TRUE(evidence.deepest_finite_m.has_value());
  EXPECT_DOUBLE_EQ(*evidence.deepest_finite_m, -0.003);

  manyforge::collision::detail::SelectedCollidingDistanceEvidence infinite_only;
  infinite_only.observe(std::numeric_limits<double>::infinity());
  EXPECT_FALSE(infinite_only.complete);
  EXPECT_FALSE(infinite_only.deepest_finite_m.has_value());
}

TEST(ManyForgeCollisionEngineTest, SnapshotPortRejectsStaleRevision) {
  const auto engine = make_engine();
  const auto snapshot = minimal_snapshot();
  CollisionQuery query;
  query.query_id = "stale-revision";
  query.collision_world_revision = snapshot.revision;
  ++query.collision_world_revision.revision_id;
  const auto result = engine->check(
      snapshot, query, CollisionPolicySnapshot{}, CollisionCancellationToken{});
  EXPECT_EQ(result.engine_status, CollisionEngineStatus::kDiagnosticIncomplete);
  EXPECT_EQ(result.decision, CollisionDecisionResult::kInconclusiveFail);
  EXPECT_EQ(result.raw_diagnostics.at("query_revision_id"), "2");
  EXPECT_EQ(result.raw_diagnostics.at("snapshot_revision_id"), "1");
}

TEST(ManyForgeCollisionEngineTest,
     SnapshotPortRejectsUnknownSnapshotFormatWithDiagnostic) {
  const auto engine = make_engine();
  auto snapshot = minimal_snapshot();
  snapshot.snapshot_format_version = 99;
  CollisionQuery query;
  query.query_id = "unknown-format";
  const auto result = check(*engine, snapshot, query);
  EXPECT_EQ(result.engine_status, CollisionEngineStatus::kDiagnosticIncomplete);
  EXPECT_EQ(result.decision, CollisionDecisionResult::kInconclusiveFail);
  EXPECT_EQ(result.raw_diagnostics.at("snapshot_format_version"), "99");
}

TEST(ManyForgeCollisionEngineTest, SnapshotPortHonorsCancellation) {
  const auto engine = make_engine();
  const auto snapshot = minimal_snapshot();
  CollisionCancellationToken cancel;
  cancel.request_cancel("owner-neutral-test");
  const auto result = check(*engine, snapshot, CollisionQuery{}, &cancel);
  EXPECT_EQ(result.engine_status, CollisionEngineStatus::kCancelled);
  EXPECT_EQ(result.decision, CollisionDecisionResult::kInconclusiveFail);
  EXPECT_EQ(result.raw_diagnostics.at("cancel_reason"), "owner-neutral-test");
}

TEST(ManyForgeCollisionEngineTest, ZeroTimeoutFailsClosedWithoutQueryPass) {
  const auto engine = make_engine();
  const auto snapshot = minimal_snapshot();
  CollisionQuery query;
  query.timeout_budget_ns = 0;
  const auto result = check(*engine, snapshot, query);
  EXPECT_EQ(result.engine_status, CollisionEngineStatus::kTimeout);
  EXPECT_EQ(result.decision, CollisionDecisionResult::kInconclusiveFail);
  EXPECT_EQ(result.raw_diagnostics.at("timeout_budget_ns"), "0");
}

TEST(ManyForgeCollisionEngineTest, MalformedQueryFailsClosedWithoutThrowing) {
  const auto engine = make_engine();
  const auto snapshot = minimal_snapshot();
  CollisionQuery query;
  query.robot_state.positions_rad = {0.0};
  const auto result = check(*engine, snapshot, query);
  EXPECT_EQ(result.engine_status, CollisionEngineStatus::kDiagnosticIncomplete);
  EXPECT_EQ(result.decision, CollisionDecisionResult::kInconclusiveFail);
  EXPECT_FALSE(result.raw_diagnostics.at("robot_state_error").empty());
}

TEST(CollisionQualificationPrimitivesTest,
     RetainedDistributionsAndBudgetsRemainExact) {
  const std::array<std::uint64_t, 4> samples{10, 20, 30, 40};
  const auto distribution =
      manyforge::qualification::summarize_distribution(samples);
  EXPECT_DOUBLE_EQ(distribution.p50, 25.0);
  EXPECT_DOUBLE_EQ(distribution.p95, 38.5);
  EXPECT_DOUBLE_EQ(distribution.p99, 39.7);
  EXPECT_DOUBLE_EQ(distribution.maximum, 40.0);
  EXPECT_DOUBLE_EQ(manyforge::qualification::median_absolute_deviation(samples),
                   10.0);

  using Schedule = manyforge::qualification::RuntimeScheduleContract;
  EXPECT_EQ(Schedule::kMonitorPeriodNs, 50'000'000);
  EXPECT_EQ(Schedule::kImmediateQueryBudgetNs, 50'000'000);
  EXPECT_EQ(Schedule::kLookaheadCycleBudgetNs, 200'000'000);
  EXPECT_EQ(Schedule::kLookaheadHorizonNs, 2'000'000'000);
  EXPECT_EQ(Schedule::kLookaheadQueryBudgetNs, 75'000'000);
  EXPECT_EQ(Schedule::kLookaheadSamples, 32U);
  EXPECT_EQ(Schedule::kTrajectoryWaypointCount, 201U);
}

TEST(CollisionQualificationPrimitivesTest,
     HistoricalFactoryCasesHaveExactRestorationAssignments) {
  EXPECT_EQ(kRetainedAcceptance.size(), 9U);
  EXPECT_EQ(kFactoryCoupledAcceptance.size(), 26U);
  EXPECT_EQ(kRetainedAcceptance.size() + kFactoryCoupledAcceptance.size(), 35U);
  for (const auto& disposition : kRetainedAcceptance) {
    EXPECT_FALSE(disposition.historical_case.empty());
    EXPECT_FALSE(disposition.restoration_gate.empty());
    EXPECT_FALSE(disposition.required_behavior.empty());
  }
  for (const auto& disposition : kFactoryCoupledAcceptance) {
    EXPECT_FALSE(disposition.historical_case.empty());
    EXPECT_FALSE(disposition.restoration_gate.empty());
    EXPECT_FALSE(disposition.required_behavior.empty());
  }
}

}  // namespace
