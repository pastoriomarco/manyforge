#include <gtest/gtest.h>

#include <manyforge_core/runtime_collision_query_session.hpp>

#include <array>
#include <atomic>
#include <cstdlib>
#include <new>

namespace {

std::atomic<bool> g_count_allocations{false};
std::atomic<std::size_t> g_allocation_count{0};

[[gnu::noinline]] void* allocate_bytes(std::size_t size) {
  return std::malloc(size == 0 ? 1 : size);
}

[[gnu::noinline]] void* allocate_aligned_bytes(std::size_t size,
                                               std::size_t alignment) {
  void* pointer = nullptr;
  if (posix_memalign(&pointer, alignment, size == 0 ? 1 : size) != 0) {
    return nullptr;
  }
  return pointer;
}

[[gnu::noinline]] void free_bytes(void* pointer) {
  std::free(pointer);
}

}  // namespace

void* operator new(std::size_t size) {
  if (g_count_allocations.load(std::memory_order_relaxed)) {
    g_allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (void* pointer = allocate_bytes(size)) {
    return pointer;
  }
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return operator new(size); }

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  if (g_count_allocations.load(std::memory_order_relaxed)) {
    g_allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
  return allocate_bytes(size);
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
  return operator new(size, tag);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
  if (g_count_allocations.load(std::memory_order_relaxed)) {
    g_allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (void* pointer =
          allocate_aligned_bytes(size, static_cast<std::size_t>(alignment))) {
    return pointer;
  }
  throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  return operator new(size, alignment);
}

void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  if (g_count_allocations.load(std::memory_order_relaxed)) {
    g_allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
  return allocate_aligned_bytes(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t& tag) noexcept {
  return operator new(size, alignment, tag);
}

void operator delete(void* pointer) noexcept { free_bytes(pointer); }
void operator delete[](void* pointer) noexcept { free_bytes(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { free_bytes(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept {
  free_bytes(pointer);
}
void operator delete(void* pointer, std::align_val_t) noexcept {
  free_bytes(pointer);
}
void operator delete[](void* pointer, std::align_val_t) noexcept {
  free_bytes(pointer);
}
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
  free_bytes(pointer);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
  free_bytes(pointer);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
  free_bytes(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
  free_bytes(pointer);
}
void operator delete(void* pointer, std::align_val_t,
                     const std::nothrow_t&) noexcept {
  free_bytes(pointer);
}
void operator delete[](void* pointer, std::align_val_t,
                       const std::nothrow_t&) noexcept {
  free_bytes(pointer);
}

namespace manyforge::core {
namespace {

class StaticRuntimePublisher final : public ICollisionWorldSnapshotPublisher {
 public:
  CollisionWorldSnapshotPtr snapshot() const override {
    return bundle ? bundle->world : nullptr;
  }

  RuntimeCollisionQueryBundlePtr published_runtime_collision_query_bundle()
      const override {
    return bundle;
  }

  RuntimeCollisionQueryBundlePtr bundle;
};

class FixedRuntimeEngine final : public ICollisionQueryEngine {
 public:
  FixedRuntimeEngine() {
    capability_.engine_id = "fixed_runtime_test_engine";
    capability_.engine_version = "test";
    capability_.library_lineage = "unit_test";
    capability_.thread_safety =
        CollisionEngineThreadSafetyClass::kSingleContextExclusive;
    capability_.mutable_context_id = "fixed_runtime_test_context";
    capability_.deterministic_ordering = true;
    capability_.runtime_query_memory_model =
        RuntimeCollisionQueryMemoryModel::kPreallocated;
    capability_.runtime_query_max_joints = 8;
    capability_.runtime_query_max_world_bodies = 32;
    capability_.runtime_query_max_collision_pairs = 64;
    capability_.runtime_query_clean_allocation_upper_bound = 0;
    capability_.runtime_query_qualified_wcet_ns = 1'000'000;
    capability_.runtime_query_snapshot_change_qualified_wcet_ns = 1'000'000;
  }

  const CollisionEngineCapabilityDescriptor& capability() const override {
    return capability_;
  }

  CollisionResult check(const CollisionWorldSnapshot&,
                        const CollisionQuery&,
                        const CollisionPolicySnapshot&,
                        const CollisionCancellationToken&) override {
    return {};
  }

  RuntimeCollisionQueryPrepareStatus prepare_runtime_query(
      std::span<const std::string> names) noexcept override {
    prepared_joint_count_ = names.size();
    return names.size() == 2 ? RuntimeCollisionQueryPrepareStatus::kReady
                             : RuntimeCollisionQueryPrepareStatus::
                                   kInvalidJointOrder;
  }

  RuntimeCollisionQueryPrepareStatus prepare_runtime_snapshot(
      const CollisionWorldSnapshot&) noexcept override {
    return RuntimeCollisionQueryPrepareStatus::kReady;
  }

  RuntimeCollisionQueryResultView check_runtime(
      const CollisionWorldSnapshot&,
      const RuntimeCollisionQueryRequest& request) noexcept override {
    ++query_count_;
    last_timeout_budget_ns_ = request.timeout_budget_ns;
    last_positions_[0] = request.positions_rad[0];
    last_positions_[1] = request.positions_rad[1];
    RuntimeCollisionQueryResultView result;
    result.status = status_;
    result.failure_reason = failure_reason_;
    result.engine_status = engine_status_;
    result.query_duration_ns = query_duration_ns_;
    if (return_oversized_collision_observations_) {
      result.collision_pairs = oversized_observations_;
    } else if (status_ == RuntimeCollisionQueryStatus::kCollision) {
      result.collision_pairs = observations_;
    }
    if (return_oversized_clearance_observations_) {
      result.clearance_pairs = oversized_observations_;
    } else if (request.require_distance && observations_[0].pair != nullptr) {
      result.clearance_pairs = observations_;
    }
    return result;
  }

  std::string engine_id() const override { return capability_.engine_id; }
  std::string engine_version() const override {
    return capability_.engine_version;
  }

  void collide(CollisionPairId pair, double clearance) {
    observe(std::move(pair), clearance, /*collision=*/true);
  }

  void observe(CollisionPairId pair, double clearance, bool collision) {
    pair_ = std::move(pair);
    if (pair_.body_b < pair_.body_a) {
      std::swap(pair_.body_a, pair_.body_b);
    }
    observations_[0] = RuntimeCollisionPairObservation{&pair_, clearance};
    status_ = collision ? RuntimeCollisionQueryStatus::kCollision
                        : RuntimeCollisionQueryStatus::kClear;
  }

  CollisionEngineCapabilityDescriptor capability_;
  RuntimeCollisionQueryStatus status_ = RuntimeCollisionQueryStatus::kClear;
  RuntimeCollisionQueryFailureReason failure_reason_ =
      RuntimeCollisionQueryFailureReason::kNone;
  CollisionEngineStatus engine_status_ = CollisionEngineStatus::kOk;
  CollisionPairId pair_;
  std::array<RuntimeCollisionPairObservation, 1> observations_{};
  std::array<RuntimeCollisionPairObservation, 65> oversized_observations_{};
  std::array<double, 2> last_positions_{};
  std::size_t prepared_joint_count_ = 0;
  std::uint64_t query_count_ = 0;
  std::int64_t last_timeout_budget_ns_ = 0;
  std::int64_t query_duration_ns_ = 1'000;
  bool return_oversized_collision_observations_ = false;
  bool return_oversized_clearance_observations_ = false;
};

RuntimeCollisionQueryBundlePtr make_bundle(CollisionPolicySnapshot policy,
                                           std::uint64_t epoch = 1) {
  auto world = std::make_shared<CollisionWorldSnapshot>();
  world->revision = {1, "runtime-world"};
  world->snapshot_format_version = 1;
  return build_runtime_collision_query_bundle(
      CollisionWorldPolicySnapshotBundle{world, std::move(policy)}, epoch);
}

RuntimeCollisionStateProjection make_projection() {
  RuntimeCollisionStateProjection projection;
  projection.source_joint_names = {"j1", "j2"};
  projection.query_joint_names = {"j1", "j2"};
  projection.template_positions_rad = {0.0, 0.0};
  projection.source_to_query_indices = {0, 1};
  return projection;
}

TEST(RuntimeCollisionQuerySessionTest,
     WarmClearTickHasZeroFrameworkAllocations) {
  CollisionPolicySnapshot policy;
  policy.revision = {1, "runtime-policy"};
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);
  ASSERT_FALSE(session.prepare(publisher.bundle).has_value());
  const std::array<double, 2> positions{0.25, -0.5};
  ASSERT_EQ(session.tick(publisher.bundle, positions, 1).status,
            RuntimeCollisionTickStatus::kClear);

  g_allocation_count.store(0, std::memory_order_relaxed);
  g_count_allocations.store(true, std::memory_order_release);
  RuntimeCollisionTickStatus last = RuntimeCollisionTickStatus::kInconclusive;
  for (int i = 0; i < 10'000; ++i) {
    last = session.tick(publisher.bundle, positions, i + 2).status;
  }
  g_count_allocations.store(false, std::memory_order_release);

  EXPECT_EQ(last, RuntimeCollisionTickStatus::kClear);
  EXPECT_EQ(g_allocation_count.load(std::memory_order_relaxed), 0u);
  EXPECT_DOUBLE_EQ(engine->last_positions_[0], positions[0]);
  EXPECT_DOUBLE_EQ(engine->last_positions_[1], positions[1]);
}

TEST(RuntimeCollisionQuerySessionTest,
     ImmutablePolicyIndexAllowsOnlyThePublishedPair) {
  const CollisionPairId allowed{"object/fixture/shape/0",
                                "robot/r/link/tool/collision/0"};
  CollisionPolicySnapshot policy;
  policy.revision = {2, "touch-policy"};
  policy.allowed_touch_link_pairs = {allowed};
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  engine->collide(allowed, -0.001);
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);
  ASSERT_FALSE(session.prepare(publisher.bundle).has_value());
  const std::array<double, 2> positions{0.0, 0.0};
  EXPECT_EQ(session.tick(publisher.bundle, positions, 1).status,
            RuntimeCollisionTickStatus::kClear);

  engine->collide({"object/unexpected/shape/0",
                   "robot/r/link/tool/collision/0"},
                  -0.001);
  const auto blocked = session.tick(publisher.bundle, positions, 2);
  EXPECT_EQ(blocked.status, RuntimeCollisionTickStatus::kCollision);
  EXPECT_EQ(blocked.reason, RuntimeCollisionTickReason::kBlockingCollision);
}

TEST(RuntimeCollisionQuerySessionTest,
     UnsupportedRuntimeCapabilityFailsBeforeMonitorArm) {
  CollisionPolicySnapshot policy;
  policy.revision = {1, "runtime-policy"};
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  engine->capability_.runtime_query_memory_model =
      RuntimeCollisionQueryMemoryModel::kUnsupported;
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);

  const auto error = session.prepare(publisher.bundle);

  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->details.at("reason"),
            "runtime_collision_query_capability_unsupported");
  EXPECT_EQ(engine->query_count_, 0u);
}

TEST(RuntimeCollisionQuerySessionTest,
     CollisionWithoutPairEvidenceFailsClosedAsInconclusive) {
  CollisionPolicySnapshot policy;
  policy.revision = {1, "runtime-policy"};
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  engine->status_ = RuntimeCollisionQueryStatus::kCollision;
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);
  ASSERT_FALSE(session.prepare(publisher.bundle).has_value());

  const std::array<double, 2> positions{0.0, 0.0};
  const auto outcome = session.tick(publisher.bundle, positions, 1);

  EXPECT_EQ(outcome.status, RuntimeCollisionTickStatus::kInconclusive);
  EXPECT_EQ(outcome.reason, RuntimeCollisionTickReason::kEngineInconclusive);
  EXPECT_EQ(outcome.engine_failure_reason,
            RuntimeCollisionQueryFailureReason::kPairDiagnosticsIncomplete);
}

TEST(RuntimeCollisionQuerySessionTest,
     QueryBudgetBelowQualifiedWcetFailsBeforeMonitorArm) {
  CollisionPolicySnapshot policy;
  policy.revision = {1, "runtime-policy"};
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  engine->capability_.runtime_query_qualified_wcet_ns = 20'000'000;
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);

  const auto error = session.prepare(publisher.bundle);

  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->details.at("reason"),
            "runtime_collision_query_budget_below_qualified_wcet");
  EXPECT_EQ(engine->query_count_, 0u);
}

TEST(RuntimeCollisionQuerySessionTest,
     ReturnedQueryTimeoutPreservesBudgetAndFailClosedClassification) {
  CollisionPolicySnapshot policy;
  policy.revision = {1, "runtime-policy"};
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  engine->status_ = RuntimeCollisionQueryStatus::kInconclusive;
  engine->failure_reason_ = RuntimeCollisionQueryFailureReason::kTimeout;
  engine->engine_status_ = CollisionEngineStatus::kTimeout;
  engine->query_duration_ns_ = 12'000'000;
  constexpr std::int64_t kImmediateQueryBudgetNs = 10'000'000;
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       kImmediateQueryBudgetNs);
  ASSERT_FALSE(session.prepare(publisher.bundle).has_value());

  const std::array<double, 2> positions{0.0, 0.0};
  const auto outcome = session.tick(publisher.bundle, positions, 1);

  EXPECT_EQ(engine->last_timeout_budget_ns_, kImmediateQueryBudgetNs);
  EXPECT_TRUE(outcome.query_executed);
  EXPECT_EQ(outcome.status, RuntimeCollisionTickStatus::kInconclusive);
  EXPECT_EQ(outcome.reason, RuntimeCollisionTickReason::kEngineInconclusive);
  EXPECT_EQ(outcome.engine_failure_reason,
            RuntimeCollisionQueryFailureReason::kTimeout);
  EXPECT_EQ(outcome.engine_status, CollisionEngineStatus::kTimeout);
  EXPECT_EQ(outcome.query_duration_ns, engine->query_duration_ns_);
}

TEST(RuntimeCollisionQuerySessionTest,
     MissingChangedSnapshotWcetFailsBeforeMonitorArm) {
  CollisionPolicySnapshot policy;
  policy.revision = {1, "runtime-policy"};
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  engine->capability_.runtime_query_snapshot_change_qualified_wcet_ns = 0;
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);

  const auto error = session.prepare(publisher.bundle);

  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->details.at("capability_status"),
            "snapshot_change_missing_wcet");
  EXPECT_EQ(engine->query_count_, 0u);
}

TEST(RuntimeCollisionQuerySessionTest,
     ContradictoryChangedSnapshotAllocationClaimFailsBeforeMonitorArm) {
  CollisionPolicySnapshot policy;
  policy.revision = {1, "runtime-policy"};
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  engine->capability_.runtime_query_snapshot_change_may_allocate = false;
  engine->capability_.runtime_query_snapshot_change_allocation_upper_bound = 1;
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);

  const auto error = session.prepare(publisher.bundle);

  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->details.at("capability_status"),
            "snapshot_change_declares_allocations_when_disabled");
  EXPECT_EQ(engine->query_count_, 0u);
}

TEST(RuntimeCollisionQuerySessionTest,
     PolicyIndexRevisionMismatchFailsBeforeMonitorArm) {
  CollisionPolicySnapshot policy;
  policy.revision = {7, "runtime-policy"};
  StaticRuntimePublisher publisher;
  const auto coherent = make_bundle(std::move(policy));
  auto mismatched_index =
      std::make_shared<CollisionPolicyIndex>(*coherent->policy_index);
  mismatched_index->source_policy_revision = {8, "different-policy"};
  auto mismatched_bundle =
      std::make_shared<RuntimeCollisionQueryBundle>(*coherent);
  mismatched_bundle->policy_index = std::move(mismatched_index);
  publisher.bundle = std::move(mismatched_bundle);
  auto engine = std::make_shared<FixedRuntimeEngine>();
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);

  const auto error = session.prepare(publisher.bundle);

  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->details.at("reason"),
            "runtime_collision_policy_index_mismatch");
  EXPECT_EQ(engine->prepared_joint_count_, 0u);
  EXPECT_EQ(engine->query_count_, 0u);
}

TEST(RuntimeCollisionQuerySessionTest,
     OversizedPolicyIndexFailsBeforeMonitorArm) {
  CollisionPolicySnapshot policy;
  policy.revision = {9, "runtime-policy"};
  StaticRuntimePublisher publisher;
  const auto coherent = make_bundle(std::move(policy));
  auto oversized_index =
      std::make_shared<CollisionPolicyIndex>(*coherent->policy_index);
  for (std::size_t i = 0; i <= 64; ++i) {
    oversized_index->touch_link_index[
        {"object/part_" + std::to_string(i), "robot/link"}] = true;
  }
  auto oversized_bundle =
      std::make_shared<RuntimeCollisionQueryBundle>(*coherent);
  oversized_bundle->policy_index = std::move(oversized_index);
  publisher.bundle = std::move(oversized_bundle);
  auto engine = std::make_shared<FixedRuntimeEngine>();
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);

  const auto error = session.prepare(publisher.bundle);

  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->details.at("reason"),
            "runtime_collision_policy_capacity_exceeded");
  EXPECT_EQ(engine->prepared_joint_count_, 0u);
  EXPECT_EQ(engine->query_count_, 0u);
}

TEST(RuntimeCollisionQuerySessionTest,
     DuplicatePolicyEntriesFailIndexCoherenceBeforeMonitorArm) {
  const CollisionPairId pair{"object/part", "robot/link"};
  CollisionPolicySnapshot policy;
  policy.revision = {15, "runtime-policy"};
  policy.allowed_touch_link_pairs = {pair, pair};
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);

  const auto error = session.prepare(publisher.bundle);

  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->details.at("reason"),
            "runtime_collision_policy_index_mismatch");
  EXPECT_EQ(engine->prepared_joint_count_, 0u);
  EXPECT_EQ(engine->query_count_, 0u);
}

TEST(RuntimeCollisionQuerySessionTest,
     OversizedActiveContactLinkSetFailsBeforeMonitorArm) {
  CollisionPolicySnapshot policy;
  policy.revision = {10, "runtime-policy"};
  ContactGraspPolicyEntry grasp;
  grasp.policy_id = "contact_grasp/part";
  grasp.selected_object_id = "part";
  for (std::size_t i = 0; i <= 8; ++i) {
    grasp.allowed_robot_links.push_back("finger_" + std::to_string(i));
  }
  policy.contact_grasps.push_back(std::move(grasp));
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  RuntimeCollisionQuerySession session(
      make_projection(), publisher, engine, "contact_grasp/part", 10'000'000);

  const auto error = session.prepare(publisher.bundle);

  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->details.at("reason"),
            "runtime_collision_policy_capacity_exceeded");
  EXPECT_EQ(engine->prepared_joint_count_, 0u);
  EXPECT_EQ(engine->query_count_, 0u);
}

TEST(RuntimeCollisionQuerySessionTest,
     OversizedFreshnessSidecarFailsBeforeMonitorArm) {
  CollisionPolicySnapshot policy;
  policy.revision = {11, "runtime-policy"};
  const auto coherent = make_bundle(std::move(policy));
  auto oversized_bundle =
      std::make_shared<RuntimeCollisionQueryBundle>(*coherent);
  for (std::size_t i = 0; i <= 32; ++i) {
    oversized_bundle->body_freshness.push_back(
        {"object/part_" + std::to_string(i), std::nullopt, std::nullopt,
         std::nullopt});
  }
  oversized_bundle->lookup_order_valid = true;
  StaticRuntimePublisher publisher;
  publisher.bundle = std::move(oversized_bundle);
  auto engine = std::make_shared<FixedRuntimeEngine>();
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);

  const auto error = session.prepare(publisher.bundle);

  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->details.at("reason"),
            "runtime_collision_bundle_capacity_exceeded");
  EXPECT_EQ(engine->prepared_joint_count_, 0u);
  EXPECT_EQ(engine->query_count_, 0u);
}

TEST(RuntimeCollisionQuerySessionTest,
     InvalidProjectionIndexFailsBeforeMonitorArm) {
  CollisionPolicySnapshot policy;
  policy.revision = {12, "runtime-policy"};
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  auto projection = make_projection();
  projection.source_to_query_indices[1] = 2;
  RuntimeCollisionQuerySession session(std::move(projection), publisher, engine,
                                       {}, 10'000'000);

  const auto error = session.prepare(publisher.bundle);

  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->details.at("reason"),
            "runtime_collision_projection_invalid_mapping");
  EXPECT_EQ(engine->prepared_joint_count_, 0u);
  EXPECT_EQ(engine->query_count_, 0u);
}

TEST(RuntimeCollisionQuerySessionTest,
     DuplicateProjectionJointNamesFailBeforeMonitorArm) {
  CollisionPolicySnapshot policy;
  policy.revision = {14, "runtime-policy"};
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  auto projection = make_projection();
  projection.source_joint_names[1] = "j1";
  projection.query_joint_names[1] = "j1";
  RuntimeCollisionQuerySession session(std::move(projection), publisher, engine,
                                       {}, 10'000'000);

  const auto error = session.prepare(publisher.bundle);

  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->details.at("reason"),
            "runtime_collision_projection_invalid_shape");
  EXPECT_EQ(engine->prepared_joint_count_, 0u);
  EXPECT_EQ(engine->query_count_, 0u);
}

TEST(RuntimeCollisionQuerySessionTest,
     OversizedEngineObservationSpansFailClosedBeforeIteration) {
  CollisionPolicySnapshot policy;
  policy.revision = {13, "runtime-policy"};
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);
  ASSERT_FALSE(session.prepare(publisher.bundle).has_value());
  const std::array<double, 2> positions{0.0, 0.0};

  engine->status_ = RuntimeCollisionQueryStatus::kCollision;
  engine->return_oversized_collision_observations_ = true;
  auto outcome = session.tick(publisher.bundle, positions, 1);
  EXPECT_EQ(outcome.status, RuntimeCollisionTickStatus::kInconclusive);
  EXPECT_EQ(outcome.reason,
            RuntimeCollisionTickReason::kObservationCapacityExceeded);
  EXPECT_EQ(outcome.engine_failure_reason,
            RuntimeCollisionQueryFailureReason::kObservationCapacityExceeded);

  engine->status_ = RuntimeCollisionQueryStatus::kClear;
  engine->return_oversized_collision_observations_ = false;
  engine->return_oversized_clearance_observations_ = true;
  outcome = session.tick(publisher.bundle, positions, 2);
  EXPECT_EQ(outcome.status, RuntimeCollisionTickStatus::kInconclusive);
  EXPECT_EQ(outcome.reason,
            RuntimeCollisionTickReason::kObservationCapacityExceeded);
  EXPECT_EQ(outcome.engine_failure_reason,
            RuntimeCollisionQueryFailureReason::kObservationCapacityExceeded);
}

TEST(RuntimeCollisionQuerySessionTest,
     ActiveContactGraspAllowsOnlyMatchingBoundedContact) {
  const CollisionPairId pair{"object/part/shape/0",
                             "robot/robot1/link/finger_left/collision/0"};
  CollisionPolicySnapshot policy;
  policy.revision = {2, "contact-grasp-policy"};
  ContactGraspPolicyEntry grasp;
  grasp.policy_id = "contact_grasp/part";
  grasp.selected_object_id = "part";
  grasp.allowed_robot_links = {"finger_left", "finger_right"};
  grasp.max_penetration_m = 0.003;
  grasp.terminal_window_start_fraction = 0.75;
  policy.contact_grasps.push_back(std::move(grasp));
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  RuntimeCollisionQuerySession session(
      make_projection(), publisher, engine, "contact_grasp/part", 10'000'000);
  ASSERT_FALSE(session.prepare(publisher.bundle).has_value());
  const std::array<double, 2> positions{0.0, 0.0};

  engine->collide(pair, -0.001);
  EXPECT_EQ(session.tick(publisher.bundle, positions, 1).status,
            RuntimeCollisionTickStatus::kClear);

  engine->collide(pair, -0.01);
  const auto blocked = session.tick(publisher.bundle, positions, 2);
  EXPECT_EQ(blocked.status, RuntimeCollisionTickStatus::kCollision);
  EXPECT_EQ(blocked.contact_grasp_violation,
            RuntimeContactGraspViolation::kPenetrationExceeded);
}

TEST(RuntimeCollisionQuerySessionTest,
     ContactReleaseStateRejectsIncreasingPenetration) {
  const CollisionPairId pair{"object/part/shape/0",
                             "robot/robot1/link/finger_left/collision/0"};
  CollisionPolicySnapshot policy;
  policy.revision = {3, "contact-release-policy"};
  SupportContactPolicyEntry release;
  release.pair = pair;
  release.mode = CollisionContactPolicyMode::kContactRelease;
  release.release_tolerance_m = 1e-5;
  release.max_start_penetration_m = 0.01;
  release.min_separation_clearance_m = 0.0;
  release.require_separation_by_end = true;
  release.require_strictly_decreasing_penetration = true;
  policy.support_contacts.push_back(std::move(release));
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  ASSERT_FALSE(
      publisher.bundle->policy->support_contacts.front().require_separation_by_end);
  ASSERT_FALSE(publisher.bundle->policy->support_contacts.front()
                   .require_strictly_decreasing_penetration);
  auto engine = std::make_shared<FixedRuntimeEngine>();
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);
  ASSERT_FALSE(session.prepare(publisher.bundle).has_value());
  const std::array<double, 2> positions{0.0, 0.0};

  engine->collide(pair, -0.001);
  EXPECT_EQ(session.tick(publisher.bundle, positions, 1).status,
            RuntimeCollisionTickStatus::kClear);
  const auto contact_state = session.contact_release_state_snapshot();
  ASSERT_TRUE(contact_state.valid);
  ASSERT_EQ(contact_state.count, 1U);
  EXPECT_EQ(contact_state.binding_owner, publisher.bundle);
  ASSERT_NE(contact_state.entries[0].pair, nullptr);
  EXPECT_EQ(*contact_state.entries[0].pair, pair);

  engine->collide(pair, -0.003);
  const auto blocked = session.tick(publisher.bundle, positions, 2);
  EXPECT_EQ(blocked.status, RuntimeCollisionTickStatus::kCollision);
  EXPECT_EQ(blocked.reason,
            RuntimeCollisionTickReason::kContactReleasePenetrationIncreased);
}

}  // namespace
}  // namespace manyforge::core
