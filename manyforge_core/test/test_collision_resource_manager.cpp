#include <gtest/gtest.h>
#include <manyforge_core/collision/collision_resource_manager.hpp>
#include <manyforge_core/execution_collision_support.hpp>

#include <string>
#include <optional>
#include <utility>

namespace manyforge::core {
namespace {

class DummyPublisher final : public ICollisionWorldSnapshotPublisher {
 public:
  CollisionWorldSnapshotPtr snapshot() const override {
    return std::make_shared<const CollisionWorldSnapshot>();
  }

  RuntimeCollisionQueryBundlePtr published_runtime_collision_query_bundle()
      const override {
    return build_runtime_collision_query_bundle(
        CollisionWorldPolicySnapshotBundle{snapshot(), CollisionPolicySnapshot{}},
        1);
  }
};

class DummyEngine final : public ICollisionQueryEngine {
 public:
  explicit DummyEngine(
      std::string id,
      CollisionEngineThreadSafetyClass thread_safety =
          CollisionEngineThreadSafetyClass::kSingleContextExclusive,
      std::string mutable_context_id = "") {
    capability_.engine_id = std::move(id);
    capability_.engine_version = "test";
    capability_.library_lineage = "unit_test";
    capability_.thread_safety = thread_safety;
    capability_.mutable_context_id = std::move(mutable_context_id);
    capability_.runtime_query_memory_model =
        RuntimeCollisionQueryMemoryModel::kMeasuredBounded;
    capability_.runtime_query_max_joints = 256;
    capability_.runtime_query_max_world_bodies = 4096;
    capability_.runtime_query_max_collision_pairs = 4096;
    capability_.runtime_query_clean_allocation_upper_bound = 1;
    capability_.runtime_query_qualified_wcet_ns = 10'000'000;
    capability_.runtime_query_snapshot_change_qualified_wcet_ns = 10'000'000;
  }

  const CollisionEngineCapabilityDescriptor& capability() const override {
    return capability_;
  }

  CollisionResult check(const CollisionWorldSnapshot& /*snapshot*/,
                        const CollisionQuery& query,
                        const CollisionPolicySnapshot& /*policy*/,
                        const CollisionCancellationToken& /*cancel*/) override {
    CollisionResult result;
    result.query_id = query.query_id;
    result.decision = CollisionDecisionResult::kPass;
    result.engine_status = CollisionEngineStatus::kOk;
    return result;
  }

  std::string engine_id() const override { return capability_.engine_id; }
  std::string engine_version() const override {
    return capability_.engine_version;
  }

  void disable_runtime_query() {
    capability_.runtime_query_memory_model =
        RuntimeCollisionQueryMemoryModel::kUnsupported;
  }

 private:
  CollisionEngineCapabilityDescriptor capability_;
};

}  // namespace

TEST(CollisionResourceManagerTest,
     SharedRuntimeAndValidationResourceConflictsOnlyWhenRuntimeMayOwnIt) {
  DummyPublisher publisher;
  auto shared = std::make_shared<DummyEngine>("shared");
  CollisionResourceManager manager;
  manager.configure(publisher, shared, {shared});

  const auto resources = manager.snapshot();

  EXPECT_TRUE(
      CollisionResourceManager::validation_pool_shares_runtime_resource(
          resources));
  EXPECT_FALSE(
      CollisionResourceManager::validation_conflicts_with_active_runtime_monitor(
          resources, ExecutionState::kIdle));
  EXPECT_TRUE(
      CollisionResourceManager::validation_conflicts_with_active_runtime_monitor(
          resources, ExecutionState::kExecuting));
  EXPECT_TRUE(
      CollisionResourceManager::validation_conflicts_with_active_runtime_monitor(
          resources, ExecutionState::kPaused));
  EXPECT_TRUE(
      CollisionResourceManager::validation_conflicts_with_active_runtime_monitor(
          resources, ExecutionState::kStopping));
}

TEST(CollisionResourceManagerTest,
     SeparateRuntimeAndValidationResourcesDoNotConflictWhileRuntimeActive) {
  DummyPublisher publisher;
  auto runtime = std::make_shared<DummyEngine>(
      "runtime",
      CollisionEngineThreadSafetyClass::kSingleContextExclusive,
      "runtime_context");
  auto validation = std::make_shared<DummyEngine>(
      "validation",
      CollisionEngineThreadSafetyClass::kSingleContextExclusive,
      "validation_context");
  CollisionResourceManager manager;
  manager.configure(publisher, runtime, {validation});

  const auto resources = manager.snapshot();

  EXPECT_FALSE(
      CollisionResourceManager::validation_pool_shares_runtime_resource(
          resources));
  EXPECT_FALSE(
      CollisionResourceManager::validation_conflicts_with_active_runtime_monitor(
          resources, ExecutionState::kExecuting));
}

TEST(CollisionResourceManagerTest, ValidationWorkerCapacityRejectsAliasedOrUnidentifiedContexts) {
  auto first = std::make_shared<DummyEngine>(
      "first", CollisionEngineThreadSafetyClass::kSingleContextExclusive, "first_context");
  auto second = std::make_shared<DummyEngine>(
      "second", CollisionEngineThreadSafetyClass::kSingleContextExclusive, "second_context");
  auto capacity = CollisionResourceManager::inspect_validation_worker_capacity({first, second});
  EXPECT_EQ(capacity.effective_contexts, 2u);
  EXPECT_FALSE(capacity.mismatch_reason.has_value());

  capacity = CollisionResourceManager::inspect_validation_worker_capacity({first, first});
  EXPECT_EQ(capacity.effective_contexts, 1u);
  EXPECT_EQ(capacity.mismatch_reason, std::optional<std::string>{"duplicate_engine_instance"});

  auto unidentified = std::make_shared<DummyEngine>(
      "unidentified", CollisionEngineThreadSafetyClass::kSingleContextExclusive);
  capacity = CollisionResourceManager::inspect_validation_worker_capacity({unidentified, second});
  EXPECT_EQ(capacity.effective_contexts, 1u);
  EXPECT_EQ(capacity.mismatch_reason, std::optional<std::string>{"missing_mutable_context_id"});
}

TEST(CollisionResourceManagerTest,
     DistinctSingleContextWrappersSharingMutableContextConflict) {
  DummyPublisher publisher;
  auto runtime = std::make_shared<DummyEngine>(
      "runtime",
      CollisionEngineThreadSafetyClass::kSingleContextExclusive,
      "shared_mutable_context");
  auto validation = std::make_shared<DummyEngine>(
      "validation",
      CollisionEngineThreadSafetyClass::kSingleContextExclusive,
      "shared_mutable_context");
  CollisionResourceManager manager;
  manager.configure(publisher, runtime, {validation});

  const auto resources = manager.snapshot();

  EXPECT_TRUE(
      CollisionResourceManager::validation_pool_shares_runtime_resource(
          resources));
  EXPECT_TRUE(
      CollisionResourceManager::validation_conflicts_with_active_runtime_monitor(
          resources, ExecutionState::kExecuting));
  const auto admission =
      CollisionResourceManager::validate_admission(
          resources, /*runtime_protection_active=*/true);
  EXPECT_FALSE(admission.admitted());
  EXPECT_EQ(
      admission.status,
      CollisionValidationAdmissionStatus::
          kRuntimeMonitorLeaseActiveSharedResource);
  EXPECT_EQ(admission.resource_conflict_reason, "shared_mutable_context_id");
}

TEST(CollisionResourceManagerTest,
     SingleContextEnginesWithoutContextIdentityConflictFailClosed) {
  DummyPublisher publisher;
  auto runtime = std::make_shared<DummyEngine>(
      "runtime", CollisionEngineThreadSafetyClass::kSingleContextExclusive);
  auto validation = std::make_shared<DummyEngine>(
      "validation", CollisionEngineThreadSafetyClass::kSingleContextExclusive);
  CollisionResourceManager manager;
  manager.configure(publisher, runtime, {validation});

  const auto resources = manager.snapshot();

  EXPECT_TRUE(
      CollisionResourceManager::validation_pool_shares_runtime_resource(
          resources));
  EXPECT_TRUE(
      CollisionResourceManager::validation_conflicts_with_active_runtime_monitor(
          resources, ExecutionState::kExecuting));
  const auto admission =
      CollisionResourceManager::validate_admission(
          resources, /*runtime_protection_active=*/true);
  EXPECT_FALSE(admission.admitted());
  EXPECT_EQ(admission.resource_conflict_reason, "missing_mutable_context_id");
}

TEST(CollisionResourceManagerTest,
     ImmutableWorkerDataEnginesMayShareModelIdentityWithoutConflict) {
  DummyPublisher publisher;
  auto runtime = std::make_shared<DummyEngine>(
      "runtime",
      CollisionEngineThreadSafetyClass::kImmutableModelWorkerData,
      "shared_immutable_model");
  auto validation = std::make_shared<DummyEngine>(
      "validation",
      CollisionEngineThreadSafetyClass::kImmutableModelWorkerData,
      "shared_immutable_model");
  CollisionResourceManager manager;
  manager.configure(publisher, runtime, {validation});

  const auto resources = manager.snapshot();

  EXPECT_FALSE(
      CollisionResourceManager::validation_pool_shares_runtime_resource(
          resources));
  EXPECT_FALSE(
      CollisionResourceManager::validation_conflicts_with_active_runtime_monitor(
          resources, ExecutionState::kExecuting));
  EXPECT_TRUE(CollisionResourceManager::validate_admission(
                  resources, /*runtime_protection_active=*/true)
                  .admitted());
}

TEST(CollisionResourceManagerTest,
     SharedRuntimeResourceAdmitsWhenRuntimeProtectionInactive) {
  DummyPublisher publisher;
  auto shared = std::make_shared<DummyEngine>("shared");
  CollisionResourceManager manager;
  manager.configure(publisher, shared, {shared});

  const auto resources = manager.snapshot();

  EXPECT_TRUE(CollisionResourceManager::validate_admission(
                  resources, /*runtime_protection_active=*/false)
                  .admitted());
}

TEST(CollisionResourceManagerTest, AdmissionReportsMissingResources) {
  DummyPublisher publisher;
  CollisionPipelineResources resources;

  auto admission = CollisionResourceManager::validate_admission(
      resources, /*runtime_protection_active=*/false);
  EXPECT_EQ(admission.status,
            CollisionValidationAdmissionStatus::kMissingSnapshotPublisher);

  resources.snapshot_publisher = &publisher;
  admission = CollisionResourceManager::validate_admission(
      resources, /*runtime_protection_active=*/false);
  EXPECT_EQ(admission.status,
            CollisionValidationAdmissionStatus::kMissingValidationResource);

  resources.validation_engines.push_back(
      std::make_shared<DummyEngine>("validation"));
  admission = CollisionResourceManager::validate_admission(
      resources, /*runtime_protection_active=*/true);
  EXPECT_EQ(admission.status,
            CollisionValidationAdmissionStatus::kMissingRuntimeResource);
}

TEST(CollisionResourceManagerTest,
     RuntimeUnsupportedEngineFailsCapabilityAdmission) {
  DummyPublisher publisher;
  auto runtime = std::make_shared<DummyEngine>("runtime");
  runtime->disable_runtime_query();
  auto validation = std::make_shared<DummyEngine>("validation");
  CollisionResourceManager manager;
  manager.configure(publisher, runtime, {validation});

  const auto admission = CollisionResourceManager::validate_admission(
      manager.snapshot(), /*runtime_protection_active=*/false);

  EXPECT_EQ(admission.status,
            CollisionValidationAdmissionStatus::
                kRuntimeQueryCapabilityUnsupported);
  EXPECT_EQ(admission.resource_conflict_reason,
            "unsupported_memory_model");
}

TEST(CollisionResourceManagerTest, LookaheadContextsCannotAliasRuntimeValidationOrEachOther) {
  DummyPublisher publisher;
  auto runtime = std::make_shared<DummyEngine>(
      "runtime", CollisionEngineThreadSafetyClass::kSingleContextExclusive, "runtime_context");
  auto validation = std::make_shared<DummyEngine>(
      "validation", CollisionEngineThreadSafetyClass::kSingleContextExclusive,
      "validation_context");
  auto lookahead = std::make_shared<DummyEngine>(
      "lookahead", CollisionEngineThreadSafetyClass::kSingleContextExclusive, "lookahead_context");
  CollisionResourceManager manager;
  manager.configure(publisher, runtime, {validation}, {lookahead});
  EXPECT_FALSE(CollisionResourceManager::runtime_lookahead_resource_conflict(manager.snapshot())
                   .has_value());

  manager.configure(publisher, runtime, {validation}, {validation});
  auto conflict = CollisionResourceManager::runtime_lookahead_resource_conflict(manager.snapshot());
  ASSERT_TRUE(conflict.has_value());
  EXPECT_EQ(*conflict, "lookahead_validation_same_engine_instance");
  const auto admission = CollisionResourceManager::validate_admission(
      manager.snapshot(), /*runtime_protection_active=*/false);
  EXPECT_EQ(admission.status,
            CollisionValidationAdmissionStatus::kRuntimeLookaheadResourceConflict);

  auto alias = std::make_shared<DummyEngine>(
      "lookahead_alias", CollisionEngineThreadSafetyClass::kSingleContextExclusive,
      "lookahead_context");
  manager.configure(publisher, runtime, {validation}, {lookahead, alias});
  conflict = CollisionResourceManager::runtime_lookahead_resource_conflict(manager.snapshot());
  ASSERT_TRUE(conflict.has_value());
  EXPECT_EQ(*conflict, "lookahead_internal_shared_mutable_context_id");
}

}  // namespace manyforge::core
