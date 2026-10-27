#include <gtest/gtest.h>

#include <manyforge_core/runtime_collision_query_session.hpp>

#include <algorithm>
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
    capability_.supports_distance_query = true;
    capability_.supported_distance_request_modes = {
        CollisionDistanceRequestMode::kNone,
        CollisionDistanceRequestMode::kSelectedCollidingPairs,
        CollisionDistanceRequestMode::kAllCollidingPairs,
        CollisionDistanceRequestMode::kAllActivePairs};
    capability_.supports_penetration_depth = true;
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
    last_distance_request_mode_ = request.distance_request_mode;
    last_selected_distance_pair_count_ = request.selected_distance_pairs.size();
    if (!request.selected_distance_pairs.empty()) {
      last_selected_distance_pair_ = request.selected_distance_pairs.front();
    }
    const bool pair_selected =
        observations_[0].pair != nullptr &&
        std::find(request.selected_distance_pairs.begin(),
                  request.selected_distance_pairs.end(),
                  *observations_[0].pair) !=
            request.selected_distance_pairs.end();
    const bool measure_pair =
        request.distance_request_mode ==
            CollisionDistanceRequestMode::kAllActivePairs ||
        (status_ == RuntimeCollisionQueryStatus::kCollision &&
         (request.distance_request_mode ==
              CollisionDistanceRequestMode::kAllCollidingPairs ||
          (request.distance_request_mode ==
               CollisionDistanceRequestMode::kSelectedCollidingPairs &&
           pair_selected)));
    returned_observations_[0] = observations_[0];
    if (!measure_pair) {
      returned_observations_[0].signed_clearance_m.reset();
    }
    RuntimeCollisionQueryResultView result;
    result.status = status_;
    result.failure_reason = failure_reason_;
    result.engine_status = engine_status_;
    result.query_duration_ns = query_duration_ns_;
    if (return_oversized_collision_observations_) {
      result.collision_pairs = oversized_observations_;
    } else if (status_ == RuntimeCollisionQueryStatus::kCollision) {
      result.collision_pairs = returned_observations_;
    }
    if (return_oversized_clearance_observations_) {
      result.clearance_pairs = oversized_observations_;
    } else if (measure_pair && observations_[0].pair != nullptr) {
      result.clearance_pairs = returned_observations_;
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

  void collide_without_clearance(CollisionPairId pair) {
    observe(std::move(pair), 0.0, /*collision=*/true);
    observations_[0].signed_clearance_m.reset();
  }

  void clear() { status_ = RuntimeCollisionQueryStatus::kClear; }

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
  std::array<RuntimeCollisionPairObservation, 1> returned_observations_{};
  std::array<RuntimeCollisionPairObservation, 65> oversized_observations_{};
  std::array<double, 2> last_positions_{};
  std::size_t prepared_joint_count_ = 0;
  std::uint64_t query_count_ = 0;
  std::int64_t last_timeout_budget_ns_ = 0;
  std::int64_t query_duration_ns_ = 1'000;
  CollisionDistanceRequestMode last_distance_request_mode_ =
      CollisionDistanceRequestMode::kNone;
  std::size_t last_selected_distance_pair_count_ = 0;
  CollisionPairId last_selected_distance_pair_;
  bool return_oversized_collision_observations_ = false;
  bool return_oversized_clearance_observations_ = false;
};

RuntimeCollisionQueryBundlePtr make_bundle(CollisionPolicySnapshot policy) {
  auto world = std::make_shared<CollisionWorldSnapshot>();
  world->revision = {1, "runtime-world"};
  world->snapshot_format_version = 1;
  auto make_body = [](std::string body_id) {
    CollisionWorldBody body;
    body.body_id = std::move(body_id);
    return body;
  };
  world->bodies = {
      make_body("component/robot1/model/robot1_model/body/finger_left/collision/0"),
      make_body("component/robot1/model/robot1_model/body/finger_right/collision/0"),
      make_body("scene/part/shape/0"),
  };
  world->body_count = static_cast<std::int64_t>(world->bodies.size());
  return build_runtime_collision_query_bundle(
      CollisionWorldPolicySnapshotBundle{world, std::move(policy)});
}

RuntimeCollisionStateProjection make_projection() {
  RuntimeCollisionStateProjection projection;
  projection.source_joint_names = {"j1", "j2"};
  projection.query_joint_names = {"j1", "j2"};
  projection.template_positions_rad = {0.0, 0.0};
  projection.source_to_query_indices = {0, 1};
  return projection;
}

ProjectionRevisionDTO make_projection_revision(std::uint64_t revision,
                                                char content_digit) {
  return ProjectionRevisionDTO{
      RetainedBundleId(
          "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
      GraphRevision(
          "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"),
      SceneRevision{
          SceneId(
              "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"),
          revision,
          ContentDigest("sha256:" + std::string(64U, content_digit))}};
}

RuntimeCollisionQueryBundlePtr with_projection_revision(
    RuntimeCollisionQueryBundlePtr source, ProjectionRevisionDTO revision) {
  auto bundle = std::make_shared<RuntimeCollisionQueryBundle>(*source);
  bundle->projection_revision = std::move(revision);
  return bundle;
}

TEST(RuntimeCollisionQueryBundleTest,
     PreservesAuthoredContactReleaseTerminalAndProgressFlags) {
  const CollisionPairId pair{"component/robot1/model/robot1_model/body/finger_left/collision/0", "scene/part/shape/0"};
  CollisionPolicySnapshot policy;
  policy.revision = {7, "preserved-release-policy"};
  SupportContactPolicyEntry release;
  release.pair = pair;
  release.require_separation_by_end = true;
  release.require_strictly_decreasing_penetration = true;
  release.min_release_progress_m = 0.004;
  policy.support_contacts.push_back(release);

  const auto bundle = make_bundle(std::move(policy));

  ASSERT_NE(bundle, nullptr);
  ASSERT_NE(bundle->policy, nullptr);
  ASSERT_EQ(bundle->policy->support_contacts.size(), 1U);
  const auto& published = bundle->policy->support_contacts.front();
  EXPECT_TRUE(published.require_separation_by_end);
  EXPECT_TRUE(published.require_strictly_decreasing_penetration);
  EXPECT_DOUBLE_EQ(published.min_release_progress_m, 0.004);
  ASSERT_NE(bundle->policy_index, nullptr);
  const auto indexed = bundle->policy_index->support_contact_index.find(pair);
  ASSERT_NE(indexed, bundle->policy_index->support_contact_index.end());
  EXPECT_TRUE(indexed->second.require_separation_by_end);
  EXPECT_TRUE(indexed->second.require_strictly_decreasing_penetration);
  EXPECT_DOUBLE_EQ(indexed->second.min_release_progress_m, 0.004);
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
     RuntimeTickKeepsNonArmFingerFromPreparedCollisionTrajectory) {
  const TrajectoryDTO arm{{}, {"j1"}, {{0.0, {0.0}, {}, {}}}};
  const TrajectoryDTO expanded{
      {}, {"j1", "j2"}, {{0.0, {0.0, 0.004}, {}, {}}}};
  auto projection = build_runtime_collision_state_projection(arm, expanded);
  ASSERT_TRUE(projection.ok()) << projection.error().message;
  CollisionPolicySnapshot policy;
  policy.revision = {1, "runtime-policy"};
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  RuntimeCollisionQuerySession session(std::move(projection.value()), publisher,
                                       engine, {}, 10'000'000);
  ASSERT_FALSE(session.prepare(publisher.bundle).has_value());

  const std::array<double, 1> arm_position{0.25};
  EXPECT_EQ(session.tick(publisher.bundle, arm_position, 1).status,
            RuntimeCollisionTickStatus::kClear);
  EXPECT_DOUBLE_EQ(engine->last_positions_[0], 0.25);
  EXPECT_DOUBLE_EQ(engine->last_positions_[1], 0.004);
}

TEST(RuntimeCollisionQuerySessionTest,
     WarmSignedReleaseTickHasZeroFrameworkAllocations) {
  const CollisionPairId pair{"component/robot1/model/robot1_model/body/finger_left/collision/0", "scene/part/shape/0"};
  CollisionPolicySnapshot policy;
  policy.revision = {2, "runtime-signed-release-policy"};
  SupportContactPolicyEntry release;
  release.pair = pair;
  release.max_start_penetration_m = 0.01;
  policy.support_contacts.push_back(release);
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  engine->collide(pair, -0.001);
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
  EXPECT_EQ(engine->last_distance_request_mode_,
            CollisionDistanceRequestMode::kSelectedCollidingPairs);
}

TEST(RuntimeCollisionQuerySessionTest,
     WarmBinaryReleaseTickHasZeroFrameworkAllocations) {
  const CollisionPairId pair{"component/robot1/model/robot1_model/body/finger_left/collision/0", "scene/part/shape/0"};
  CollisionPolicySnapshot policy;
  policy.revision = {3, "runtime-binary-release-policy"};
  SupportContactPolicyEntry release;
  release.pair = pair;
  release.release_evidence_mode = ContactReleaseEvidenceMode::kBinaryContact;
  release.enforce_max_start_penetration = false;
  release.max_start_penetration_m = 0.0;
  release.release_tolerance_m = 0.0;
  release.min_release_progress_m = 0.0;
  release.require_strictly_decreasing_penetration = false;
  policy.support_contacts.push_back(release);
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  engine->collide_without_clearance(pair);
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
  EXPECT_EQ(engine->last_distance_request_mode_,
            CollisionDistanceRequestMode::kNone);
}

TEST(RuntimeCollisionQuerySessionTest,
     WarmBoundedPenetrationTickHasZeroFrameworkAllocations) {
  const CollisionPairId pair{"component/robot1/model/robot1_model/body/finger_left/collision/0", "scene/part/shape/0"};
  CollisionPolicySnapshot policy;
  policy.revision = {4, "runtime-bounded-contact-policy"};
  SupportContactPolicyEntry contact;
  contact.pair = pair;
  contact.mode = CollisionContactPolicyMode::kBoundedPenetration;
  contact.max_contact_penetration_m = 0.002;
  policy.support_contacts.push_back(contact);
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  engine->collide(pair, -0.001);
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
  EXPECT_EQ(engine->last_distance_request_mode_,
            CollisionDistanceRequestMode::kSelectedCollidingPairs);
}

TEST(RuntimeCollisionQuerySessionTest,
     ImmutablePolicyIndexAllowsOnlyThePublishedPair) {
  const CollisionPairId allowed{"component/r/model/r_model/body/tool/collision/0", "scene/fixture/shape/0"};
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

  engine->collide({"scene/unexpected/shape/0",
                   "component/r/model/r_model/body/tool/collision/0"},
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
     MissingDistanceScopeDeclarationFailsGeneralCapabilityAdmission) {
  CollisionPolicySnapshot policy;
  policy.revision = {1, "runtime-policy"};
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  engine->capability_.supported_distance_request_modes.clear();
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);

  const auto error = session.prepare(publisher.bundle);

  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->details.at("reason"),
            "runtime_collision_query_capability_unsupported");
  EXPECT_EQ(error->details.at("capability_status"),
            "invalid_distance_request_modes");
  EXPECT_EQ(engine->prepared_joint_count_, 0u);
  EXPECT_EQ(engine->query_count_, 0u);
}

TEST(RuntimeCollisionQuerySessionTest,
     DistanceScopeCapabilityDeclarationMustBeSortedUniqueAndConsistent) {
  FixedRuntimeEngine engine;
  auto capability = engine.capability_;

  capability.supported_distance_request_modes = {
      CollisionDistanceRequestMode::kSelectedCollidingPairs,
      CollisionDistanceRequestMode::kNone};
  EXPECT_FALSE(collision_distance_request_modes_valid(capability));
  EXPECT_EQ(runtime_collision_query_capability_status(capability),
            RuntimeCollisionQueryCapabilityStatus::
                kInvalidDistanceRequestModes);

  capability.supported_distance_request_modes = {
      CollisionDistanceRequestMode::kNone,
      CollisionDistanceRequestMode::kNone};
  EXPECT_FALSE(collision_distance_request_modes_valid(capability));

  capability.supported_distance_request_modes = {
      CollisionDistanceRequestMode::kNone,
      CollisionDistanceRequestMode::kSelectedCollidingPairs};
  capability.supports_distance_query = false;
  EXPECT_FALSE(collision_distance_request_modes_valid(capability));
}

TEST(RuntimeCollisionQuerySessionTest,
     SignedPolicyRejectsAllActiveOnlyEngineWithoutScopeFallback) {
  const CollisionPairId pair{"component/robot1/model/robot1_model/body/finger_left/collision/0", "scene/part/shape/0"};
  CollisionPolicySnapshot policy;
  policy.revision = {2, "runtime-signed-release-policy"};
  SupportContactPolicyEntry release;
  release.pair = pair;
  release.max_start_penetration_m = 0.01;
  policy.support_contacts.push_back(release);
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  engine->capability_.supported_distance_request_modes = {
      CollisionDistanceRequestMode::kNone,
      CollisionDistanceRequestMode::kAllActivePairs};
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);

  const auto error = session.prepare(publisher.bundle);

  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->details.at("reason"),
            "runtime_collision_distance_request_mode_unsupported");
  EXPECT_EQ(engine->prepared_joint_count_, 0u);
  EXPECT_EQ(engine->query_count_, 0u);
}

TEST(RuntimeCollisionQuerySessionTest,
     SignedPolicyReportsMissingPenetrationCapabilityPrecisely) {
  const CollisionPairId pair{"component/robot1/model/robot1_model/body/finger_left/collision/0", "scene/part/shape/0"};
  CollisionPolicySnapshot policy;
  policy.revision = {3, "runtime-signed-release-missing-depth-policy"};
  SupportContactPolicyEntry release;
  release.pair = pair;
  release.max_start_penetration_m = 0.01;
  policy.support_contacts.push_back(release);
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  engine->capability_.supports_penetration_depth = false;
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);

  const auto error = session.prepare(publisher.bundle);

  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->details.at("reason"),
            "runtime_collision_required_capability_unsupported");
  EXPECT_EQ(error->details.at("required_capability"),
            "penetration_depth");
  EXPECT_EQ(engine->prepared_joint_count_, 0u);
  EXPECT_EQ(engine->query_count_, 0u);
}

TEST(RuntimeCollisionQuerySessionTest,
     BinaryPolicyAdmitsExplicitCollisionOnlyNoneScope) {
  const CollisionPairId pair{"component/robot1/model/robot1_model/body/finger_left/collision/0", "scene/part/shape/0"};
  CollisionPolicySnapshot policy;
  policy.revision = {3, "runtime-binary-release-policy"};
  SupportContactPolicyEntry release;
  release.pair = pair;
  release.release_evidence_mode = ContactReleaseEvidenceMode::kBinaryContact;
  release.enforce_max_start_penetration = false;
  release.max_start_penetration_m = 0.0;
  release.release_tolerance_m = 0.0;
  release.min_release_progress_m = 0.0;
  release.require_strictly_decreasing_penetration = false;
  policy.support_contacts.push_back(release);
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  engine->capability_.supports_distance_query = false;
  engine->capability_.supports_penetration_depth = false;
  engine->capability_.supported_distance_request_modes = {
      CollisionDistanceRequestMode::kNone};
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);

  EXPECT_FALSE(session.prepare(publisher.bundle).has_value());
  const std::array<double, 2> positions{0.0, 0.0};
  const auto outcome = session.tick(publisher.bundle, positions, 1);
  EXPECT_EQ(outcome.status, RuntimeCollisionTickStatus::kClear);
  EXPECT_EQ(engine->last_distance_request_mode_,
            CollisionDistanceRequestMode::kNone);
}

TEST(RuntimeCollisionQuerySessionTest,
     BoundedPolicyWithReleaseFieldMixtureFailsBeforeMonitorArm) {
  const CollisionPairId pair{"component/robot1/model/robot1_model/body/finger_left/collision/0", "scene/part/shape/0"};
  CollisionPolicySnapshot policy;
  policy.revision = {4, "runtime-invalid-bounded-policy"};
  SupportContactPolicyEntry bounded;
  bounded.pair = pair;
  bounded.mode = CollisionContactPolicyMode::kBoundedPenetration;
  bounded.max_contact_penetration_m = 0.002;
  bounded.release_tolerance_m = 0.001;
  policy.support_contacts.push_back(bounded);
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);

  const auto error = session.prepare(publisher.bundle);

  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->details.at("reason"),
            "runtime_contact_release_policy_invalid");
  EXPECT_EQ(engine->prepared_joint_count_, 0u);
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
        {"component/arm/model/arm_model/body/link/collision/0",
         "scene/part_" + std::to_string(i) + "/shape/0"}] = true;
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
  const CollisionPairId pair{"component/arm/model/arm_model/body/link/collision/0", "scene/part/shape/0"};
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
     RuntimeBundlePreselectsMetricPairsAndExcludesBinaryPairs) {
  const CollisionPairId bounded_pair{"scene/fixture/shape/0",
                                     "scene/insert/shape/0"};
  const CollisionPairId binary_pair{"scene/binary/shape/0", "scene/table/shape/0"};
  CollisionPolicySnapshot policy;
  policy.revision = {2, "mixed-runtime-distance-selection"};

  SupportContactPolicyEntry bounded;
  bounded.pair = bounded_pair;
  bounded.mode = CollisionContactPolicyMode::kBoundedPenetration;
  bounded.max_contact_penetration_m = 0.002;
  policy.support_contacts.push_back(bounded);

  SupportContactPolicyEntry binary;
  binary.pair = binary_pair;
  binary.mode = CollisionContactPolicyMode::kContactRelease;
  binary.release_evidence_mode = ContactReleaseEvidenceMode::kBinaryContact;
  binary.enforce_max_start_penetration = false;
  binary.max_start_penetration_m = 0.0;
  binary.release_tolerance_m = 0.0;
  binary.min_release_progress_m = 0.0;
  binary.require_strictly_decreasing_penetration = false;
  policy.support_contacts.push_back(binary);

  ContactGraspPolicyEntry grasp;
  grasp.policy_id = "contact_grasp/part";
  grasp.selected_object_id = "part";
  grasp.allowed_robot_links = {"finger_left", "finger_right"};
  grasp.max_penetration_m = 0.003;
  grasp.terminal_window_start_fraction = 0.75;
  policy.contact_grasps.push_back(grasp);

  const auto bundle = make_bundle(std::move(policy));
  ASSERT_TRUE(bundle->distance_selection_order_valid);
  const auto& selected =
      bundle->contact_grasp_selected_distance_pairs.at("contact_grasp/part");
  EXPECT_NE(std::find(selected.begin(), selected.end(), bounded_pair),
            selected.end());
  EXPECT_EQ(std::find(selected.begin(), selected.end(), binary_pair),
            selected.end());
  EXPECT_EQ(selected.size(), 3u);
}

TEST(RuntimeCollisionQuerySessionTest,
     ActiveContactGraspAllowsOnlyMatchingBoundedContact) {
  const CollisionPairId pair{"component/robot1/model/robot1_model/body/finger_left/collision/0", "scene/part/shape/0"};
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
  EXPECT_EQ(engine->last_distance_request_mode_,
            CollisionDistanceRequestMode::kSelectedCollidingPairs);
  EXPECT_EQ(engine->last_selected_distance_pair_count_, 2u);

  engine->collide(pair, -0.01);
  const auto blocked = session.tick(publisher.bundle, positions, 2);
  EXPECT_EQ(blocked.status, RuntimeCollisionTickStatus::kCollision);
  EXPECT_EQ(blocked.contact_grasp_violation,
            RuntimeContactGraspViolation::kPenetrationExceeded);
}

TEST(RuntimeCollisionQuerySessionTest,
     ActiveContactGraspRejectsContradictoryPositiveClearance) {
  const CollisionPairId pair{"component/robot1/model/robot1_model/body/finger_left/collision/0", "scene/part/shape/0"};
  CollisionPolicySnapshot policy;
  policy.revision = {2, "contact-grasp-positive-clearance"};
  ContactGraspPolicyEntry grasp;
  grasp.policy_id = "contact_grasp/part";
  grasp.selected_object_id = "part";
  grasp.allowed_robot_links = {"finger_left"};
  grasp.max_penetration_m = 0.003;
  grasp.terminal_window_start_fraction = 0.75;
  policy.contact_grasps.push_back(std::move(grasp));
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  RuntimeCollisionQuerySession session(
      make_projection(), publisher, engine, "contact_grasp/part", 10'000'000);
  ASSERT_FALSE(session.prepare(publisher.bundle).has_value());

  engine->collide(pair, 0.001);
  const std::array<double, 2> positions{0.0, 0.0};
  const auto blocked = session.tick(publisher.bundle, positions, 1);

  EXPECT_EQ(blocked.status, RuntimeCollisionTickStatus::kInconclusive);
  EXPECT_EQ(blocked.reason, RuntimeCollisionTickReason::kEngineInconclusive);
  EXPECT_EQ(blocked.engine_failure_reason,
            RuntimeCollisionQueryFailureReason::kPairDiagnosticsIncomplete);
  EXPECT_EQ(blocked.engine_status,
            CollisionEngineStatus::kDiagnosticIncomplete);
  EXPECT_EQ(blocked.contact_grasp_violation,
            RuntimeContactGraspViolation::kMissingClearance);
}

TEST(RuntimeCollisionQuerySessionTest,
     ContactReleaseStateRejectsIncreasingPenetration) {
  const CollisionPairId pair{"component/robot1/model/robot1_model/body/finger_left/collision/0", "scene/part/shape/0"};
  CollisionPolicySnapshot policy;
  policy.revision = {3, "contact-release-policy"};
  SupportContactPolicyEntry release;
  release.pair = pair;
  release.mode = CollisionContactPolicyMode::kContactRelease;
  release.release_tolerance_m = 1e-5;
  release.max_start_penetration_m = 0.01;
  release.require_separation_by_end = true;
  release.require_strictly_decreasing_penetration = true;
  policy.support_contacts.push_back(std::move(release));
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  ASSERT_TRUE(
      publisher.bundle->policy->support_contacts.front().require_separation_by_end);
  ASSERT_TRUE(publisher.bundle->policy->support_contacts.front()
                  .require_strictly_decreasing_penetration);
  auto engine = std::make_shared<FixedRuntimeEngine>();
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);
  ASSERT_FALSE(session.prepare(publisher.bundle).has_value());
  const std::array<double, 2> positions{0.0, 0.0};

  engine->collide(pair, -0.001);
  EXPECT_EQ(session.tick(publisher.bundle, positions, 1).status,
            RuntimeCollisionTickStatus::kClear);
  EXPECT_EQ(engine->last_distance_request_mode_,
            CollisionDistanceRequestMode::kSelectedCollidingPairs);
  ASSERT_EQ(engine->last_selected_distance_pair_count_, 1U);
  EXPECT_EQ(engine->last_selected_distance_pair_, pair);
  const auto contact_state = session.contact_release_state_snapshot();
  ASSERT_TRUE(contact_state.valid);
  ASSERT_EQ(contact_state.count, 1U);
  EXPECT_EQ(contact_state.binding_owner, publisher.bundle);
  ASSERT_NE(contact_state.entries[0].pair, nullptr);
  EXPECT_EQ(*contact_state.entries[0].pair, pair);
  // A current-state tick advances release history but is not a trajectory-end
  // proof, even though the immutable policy retains its terminal requirement.
  EXPECT_TRUE(contact_state.entries[0].release_state.in_contact);

  engine->collide(pair, -0.003);
  const auto blocked = session.tick(publisher.bundle, positions, 2);
  EXPECT_EQ(blocked.status, RuntimeCollisionTickStatus::kCollision);
  EXPECT_EQ(blocked.reason,
            RuntimeCollisionTickReason::kContactReleasePenetrationIncreased);
}

TEST(RuntimeCollisionQuerySessionTest,
     CommandCapableSessionFollowsRuleChangesAndCarriesContactReleaseHistory) {
  const CollisionPairId released_pair{
      "component/robot1/model/robot1_model/body/finger_left/collision/0",
      "scene/part/shape/0"};
  const CollisionPairId added_pair{
      "component/robot1/model/robot1_model/body/finger_right/collision/0",
      "scene/part/shape/0"};
  SupportContactPolicyEntry release;
  release.pair = released_pair;
  release.max_start_penetration_m = 0.01;
  SupportContactPolicyEntry added = release;
  added.pair = added_pair;
  const auto entry_for = [](const RuntimeContactReleaseStateSnapshot& snapshot,
                            const CollisionPairId& pair) {
    for (std::size_t i = 0; i < snapshot.count; ++i) {
      if (snapshot.entries[i].pair != nullptr && *snapshot.entries[i].pair == pair) {
        return &snapshot.entries[i];
      }
    }
    return static_cast<const RuntimeContactReleaseStateEntry*>(nullptr);
  };

  CollisionPolicySnapshot admission_policy;
  admission_policy.revision = {5, "admission-contact-release-policy"};
  admission_policy.support_contacts.push_back(release);
  const auto admission_revision = make_projection_revision(1U, 'd');
  const auto admission_bundle = with_projection_revision(
      make_bundle(std::move(admission_policy)), admission_revision);
  StaticRuntimePublisher publisher;
  publisher.bundle = admission_bundle;
  auto engine = std::make_shared<FixedRuntimeEngine>();
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);
  ASSERT_FALSE(session.prepare(admission_bundle).has_value());
  const std::array<double, 2> positions{0.0, 0.0};

  engine->collide(released_pair, -0.002);
  EXPECT_EQ(session.tick(publisher.bundle, positions, 1).status,
            RuntimeCollisionTickStatus::kClear);
  engine->clear();
  EXPECT_EQ(session.tick(publisher.bundle, positions, 2).status,
            RuntimeCollisionTickStatus::kClear);

  // New rules mid-motion: the unchanged pair keeps its released history, the
  // new pair starts fresh (it may start in contact), and the start-of-motion
  // baseline is not re-counted.
  CollisionPolicySnapshot extended_policy;
  extended_policy.revision = {6, "attach-adds-support-contact"};
  extended_policy.support_contacts = {release, added};
  publisher.bundle = with_projection_revision(
      make_bundle(std::move(extended_policy)),
      make_projection_revision(2U, 'e'));
  engine->collide(added_pair, -0.002);
  const auto rebound = session.tick(publisher.bundle, positions, 3);
  EXPECT_EQ(rebound.status, RuntimeCollisionTickStatus::kClear);
  EXPECT_EQ(rebound.initial_contact_pair_count, 1U);
  EXPECT_EQ(rebound.current_contact_pair_count, 1U);
  const auto extended = session.contact_release_state_snapshot();
  ASSERT_TRUE(extended.valid);
  EXPECT_EQ(extended.binding_owner, publisher.bundle);
  ASSERT_EQ(extended.count, 2U);
  const auto* carried = entry_for(extended, released_pair);
  const auto* fresh = entry_for(extended, added_pair);
  ASSERT_NE(carried, nullptr);
  ASSERT_NE(fresh, nullptr);
  EXPECT_TRUE(carried->release_state.separated);
  EXPECT_EQ(carried->support_state, SupportContactEvaluationState::kSeparated);
  EXPECT_TRUE(fresh->release_state.started_in_contact);
  EXPECT_FALSE(fresh->release_state.separated);

  engine->collide(released_pair, -0.001);
  const auto recontact = session.tick(publisher.bundle, positions, 4);
  EXPECT_EQ(recontact.status, RuntimeCollisionTickStatus::kCollision);
  EXPECT_EQ(recontact.reason, RuntimeCollisionTickReason::kContactReleaseRecontact);

  // A changed rule starts fresh and a removed pair drops out.
  CollisionPolicySnapshot changed_policy;
  changed_policy.revision = {7, "changed-support-contact"};
  SupportContactPolicyEntry changed = release;
  changed.max_start_penetration_m = 0.02;
  changed_policy.support_contacts = {changed};
  publisher.bundle = with_projection_revision(
      make_bundle(std::move(changed_policy)),
      make_projection_revision(3U, 'f'));
  engine->collide(released_pair, -0.001);
  EXPECT_EQ(session.tick(publisher.bundle, positions, 5).status,
            RuntimeCollisionTickStatus::kClear);
  const auto changed_state = session.contact_release_state_snapshot();
  ASSERT_EQ(changed_state.count, 1U);
  const auto* restarted = entry_for(changed_state, released_pair);
  ASSERT_NE(restarted, nullptr);
  EXPECT_TRUE(restarted->release_state.started_in_contact);
  EXPECT_FALSE(restarted->release_state.separated);
  EXPECT_EQ(entry_for(changed_state, added_pair), nullptr);
}

TEST(RuntimeCollisionQuerySessionTest,
     SignedReleaseTreatsAbsentPairAsSeparatedWithoutDistance) {
  const CollisionPairId pair{"component/robot1/model/robot1_model/body/finger_left/collision/0", "scene/part/shape/0"};
  CollisionPolicySnapshot policy;
  policy.revision = {4, "signed-release-policy"};
  SupportContactPolicyEntry release;
  release.pair = pair;
  release.max_start_penetration_m = 0.01;
  policy.support_contacts.push_back(release);
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);
  ASSERT_FALSE(session.prepare(publisher.bundle).has_value());
  const std::array<double, 2> positions{0.0, 0.0};

  engine->collide(pair, -0.002);
  EXPECT_EQ(session.tick(publisher.bundle, positions, 1).status,
            RuntimeCollisionTickStatus::kClear);
  engine->clear();
  const auto separated = session.tick(publisher.bundle, positions, 2);

  EXPECT_EQ(separated.status, RuntimeCollisionTickStatus::kClear);
  EXPECT_EQ(engine->last_distance_request_mode_,
            CollisionDistanceRequestMode::kSelectedCollidingPairs);
  const auto snapshot = session.contact_release_state_snapshot();
  ASSERT_TRUE(snapshot.valid);
  ASSERT_EQ(snapshot.count, 1U);
  EXPECT_FALSE(snapshot.entries[0].release_state.in_contact);
  EXPECT_TRUE(snapshot.entries[0].release_state.separated);
}

TEST(RuntimeCollisionQuerySessionTest,
     BinaryReleaseRequestsNoDistanceAndRejectsRecontact) {
  const CollisionPairId pair{"component/robot1/model/robot1_model/body/finger_left/collision/0", "scene/part/shape/0"};
  CollisionPolicySnapshot policy;
  policy.revision = {5, "binary-release-policy"};
  SupportContactPolicyEntry release;
  release.pair = pair;
  release.release_evidence_mode = ContactReleaseEvidenceMode::kBinaryContact;
  release.enforce_max_start_penetration = false;
  release.release_tolerance_m = 0.0;
  release.require_strictly_decreasing_penetration = false;
  policy.support_contacts.push_back(release);
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);
  ASSERT_FALSE(session.prepare(publisher.bundle).has_value());
  const std::array<double, 2> positions{0.0, 0.0};

  engine->collide(pair, -0.002);
  EXPECT_EQ(session.tick(publisher.bundle, positions, 1).status,
            RuntimeCollisionTickStatus::kClear);
  EXPECT_EQ(engine->last_distance_request_mode_,
            CollisionDistanceRequestMode::kNone);
  EXPECT_EQ(engine->last_selected_distance_pair_count_, 0U);

  engine->collide(pair, -0.004);
  EXPECT_EQ(session.tick(publisher.bundle, positions, 2).status,
            RuntimeCollisionTickStatus::kClear);
  engine->clear();
  EXPECT_EQ(session.tick(publisher.bundle, positions, 3).status,
            RuntimeCollisionTickStatus::kClear);
  engine->collide(pair, -0.001);
  const auto recontact = session.tick(publisher.bundle, positions, 4);

  EXPECT_EQ(recontact.status, RuntimeCollisionTickStatus::kCollision);
  EXPECT_EQ(recontact.reason,
            RuntimeCollisionTickReason::kContactReleaseRecontact);
}

TEST(RuntimeCollisionQuerySessionTest,
     BinaryReleaseRejectsContactThatWasAbsentAtRuntimeBaseline) {
  const CollisionPairId pair{"component/robot1/model/robot1_model/body/finger_left/collision/0", "scene/part/shape/0"};
  CollisionPolicySnapshot policy;
  policy.revision = {6, "binary-new-contact-policy"};
  SupportContactPolicyEntry release;
  release.pair = pair;
  release.release_evidence_mode = ContactReleaseEvidenceMode::kBinaryContact;
  release.enforce_max_start_penetration = false;
  release.release_tolerance_m = 0.0;
  release.require_strictly_decreasing_penetration = false;
  policy.support_contacts.push_back(release);
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);
  ASSERT_FALSE(session.prepare(publisher.bundle).has_value());
  const std::array<double, 2> positions{0.0, 0.0};

  engine->clear();
  EXPECT_EQ(session.tick(publisher.bundle, positions, 1).status,
            RuntimeCollisionTickStatus::kClear);
  engine->collide(pair, -0.001);
  const auto new_contact = session.tick(publisher.bundle, positions, 2);

  EXPECT_EQ(new_contact.status, RuntimeCollisionTickStatus::kCollision);
  EXPECT_EQ(new_contact.reason,
            RuntimeCollisionTickReason::kContactReleaseNewContact);
}

TEST(RuntimeCollisionQuerySessionTest,
     BoundedPenetrationAllowsAppearanceRecontactAndFinalContactWithinBound) {
  const CollisionPairId pair{"scene/fixture/shape/0", "scene/insert/shape/0"};
  CollisionPolicySnapshot policy;
  policy.revision = {7, "bounded-contact-policy"};
  SupportContactPolicyEntry contact;
  contact.pair = pair;
  contact.mode = CollisionContactPolicyMode::kBoundedPenetration;
  contact.max_contact_penetration_m = 0.002;
  policy.support_contacts.push_back(contact);
  StaticRuntimePublisher publisher;
  publisher.bundle = make_bundle(std::move(policy));
  auto engine = std::make_shared<FixedRuntimeEngine>();
  RuntimeCollisionQuerySession session(make_projection(), publisher, engine, {},
                                       10'000'000);
  ASSERT_FALSE(session.prepare(publisher.bundle).has_value());
  const std::array<double, 2> positions{0.0, 0.0};

  engine->clear();
  EXPECT_EQ(session.tick(publisher.bundle, positions, 1).status,
            RuntimeCollisionTickStatus::kClear);
  EXPECT_EQ(engine->last_distance_request_mode_,
            CollisionDistanceRequestMode::kSelectedCollidingPairs);

  engine->collide(pair, -0.002);
  EXPECT_EQ(session.tick(publisher.bundle, positions, 2).status,
            RuntimeCollisionTickStatus::kClear);
  const auto appeared = session.contact_release_state_snapshot();
  ASSERT_TRUE(appeared.valid);
  ASSERT_EQ(appeared.count, 1u);
  EXPECT_EQ(appeared.entries[0].support_state,
            SupportContactEvaluationState::kContactActive);
  engine->clear();
  EXPECT_EQ(session.tick(publisher.bundle, positions, 3).status,
            RuntimeCollisionTickStatus::kClear);
  engine->collide(pair, -0.0015);
  EXPECT_EQ(session.tick(publisher.bundle, positions, 4).status,
            RuntimeCollisionTickStatus::kClear);

  engine->collide_without_clearance(pair);
  const auto missing = session.tick(publisher.bundle, positions, 5);
  EXPECT_EQ(missing.status, RuntimeCollisionTickStatus::kInconclusive);
  EXPECT_EQ(missing.reason, RuntimeCollisionTickReason::kEngineInconclusive);
  EXPECT_EQ(missing.engine_failure_reason,
            RuntimeCollisionQueryFailureReason::kPairDiagnosticsIncomplete);
  EXPECT_EQ(missing.engine_status,
            CollisionEngineStatus::kDiagnosticIncomplete);

  engine->collide(pair, -0.003);
  const auto exceeded = session.tick(publisher.bundle, positions, 6);
  EXPECT_EQ(exceeded.status, RuntimeCollisionTickStatus::kCollision);
  EXPECT_EQ(exceeded.reason,
            RuntimeCollisionTickReason::kBoundedContactPenetrationExceeded);
  EXPECT_EQ(exceeded.pair_classification,
            CollisionPairClassification::kBlockingCollision);
  ASSERT_TRUE(exceeded.max_contact_penetration_m.has_value());
  EXPECT_DOUBLE_EQ(*exceeded.max_contact_penetration_m, 0.002);
  ASSERT_TRUE(exceeded.contact_policy_mode.has_value());
  EXPECT_EQ(*exceeded.contact_policy_mode,
            CollisionContactPolicyMode::kBoundedPenetration);
  EXPECT_FALSE(exceeded.release_evidence_mode.has_value());
  EXPECT_TRUE(exceeded.contact_present);
  EXPECT_EQ(exceeded.initial_contact_pair_count, 0u);
  EXPECT_EQ(exceeded.current_contact_pair_count, 1u);
  EXPECT_EQ(exceeded.distance_request_mode,
            CollisionDistanceRequestMode::kSelectedCollidingPairs);
  EXPECT_EQ(exceeded.evaluated_distance_pair_count, 1u);
  ASSERT_TRUE(exceeded.current_penetration_m.has_value());
  EXPECT_DOUBLE_EQ(*exceeded.current_penetration_m, 0.003);
}

}  // namespace
}  // namespace manyforge::core
