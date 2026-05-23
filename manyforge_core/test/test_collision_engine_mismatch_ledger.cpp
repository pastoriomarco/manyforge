// Chunk 8 groundwork tests for CollisionEngineMismatchLedger and the
// CollisionEngineCapabilityDescriptor schema. Behaviour under test:
//   - bounded retention drops oldest first;
//   - recent(0) returns every retained record newest-first;
//   - thread-safe concurrent pushes do not corrupt internal state.

#include <manyforge_core/collision/collision_engine_mismatch_ledger.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

namespace manyforge::core {
namespace {

CollisionEngineMismatchRecord make_record(int seq) {
  CollisionEngineMismatchRecord r;
  r.mismatch_id = "mm-" + std::to_string(seq);
  r.emitted_at_ns = seq * 1000;
  r.authoritative_engine_id = "roboplan_native";
  r.authoritative_engine_version = "0.x";
  r.secondary_engine_id = "direct_roboplan_robot_only_compare";
  r.secondary_engine_version = "0.x";
  CollisionWorldRevision rev;
  rev.revision_id = 42;
  r.collision_world_revision = rev;
  r.policy_revision = "7";
  r.waypoint_index = seq;
  r.state_time_ns = seq * 10;
  r.disposition = CollisionEngineMismatchDisposition::kTolerated;
  return r;
}

TEST(CollisionEngineMismatchLedgerTest,
     RecentNewestFirstWithBoundedRetention) {
  CollisionEngineMismatchLedger ledger(4);
  for (int i = 0; i < 6; ++i) {
    ledger.push(make_record(i));
  }
  EXPECT_EQ(ledger.total_pushed(), 6u);
  auto recent = ledger.recent(0);
  ASSERT_EQ(recent.size(), 4u);
  EXPECT_EQ(recent[0].mismatch_id, "mm-5");
  EXPECT_EQ(recent[1].mismatch_id, "mm-4");
  EXPECT_EQ(recent[2].mismatch_id, "mm-3");
  EXPECT_EQ(recent[3].mismatch_id, "mm-2");
}

TEST(CollisionEngineMismatchLedgerTest, LimitParameterHonored) {
  CollisionEngineMismatchLedger ledger(16);
  for (int i = 0; i < 10; ++i) ledger.push(make_record(i));
  auto recent = ledger.recent(3);
  ASSERT_EQ(recent.size(), 3u);
  EXPECT_EQ(recent[0].mismatch_id, "mm-9");
  EXPECT_EQ(recent[2].mismatch_id, "mm-7");
}

TEST(CollisionEngineMismatchLedgerTest, ClearKeepsTotalCounter) {
  CollisionEngineMismatchLedger ledger(8);
  ledger.push(make_record(1));
  ledger.push(make_record(2));
  EXPECT_EQ(ledger.total_pushed(), 2u);
  ledger.clear();
  EXPECT_EQ(ledger.total_pushed(), 2u);
  EXPECT_TRUE(ledger.recent(0).empty());
}

TEST(CollisionEngineMismatchLedgerTest, ZeroCapacityDropsButCountsRecords) {
  CollisionEngineMismatchLedger ledger(0);
  ledger.push(make_record(1));
  ledger.push(make_record(2));
  EXPECT_EQ(ledger.total_pushed(), 2u);
  EXPECT_TRUE(ledger.recent(0).empty());
}

TEST(CollisionEngineMismatchLedgerTest, ConcurrentPushesAreRaceFree) {
  CollisionEngineMismatchLedger ledger(2048);
  std::atomic<int> counter{0};
  auto worker = [&] {
    for (int i = 0; i < 100; ++i) {
      ledger.push(make_record(counter.fetch_add(1)));
    }
  };
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) threads.emplace_back(worker);
  for (auto& t : threads) t.join();
  EXPECT_EQ(ledger.total_pushed(), 800u);
  EXPECT_EQ(ledger.recent(0).size(), 800u);
}

TEST(CollisionEngineMismatchLedgerTest,
     CapabilityDescriptorRoundTripsExpectedFields) {
  CollisionEngineCapabilityDescriptor d;
  d.engine_id = "direct_roboplan_robot_only_compare";
  d.engine_version = "0.x";
  d.library_lineage = "coal";
  d.supported_geometry_kinds = {"primitive", "convex_mesh"};
  d.supports_distance_query = false;
  d.supports_contact_points = false;
  d.supports_penetration_depth = false;
  d.supports_continuous_collision = false;
  d.thread_safety =
      CollisionEngineThreadSafetyClass::kImmutableModelWorkerData;
  d.deterministic_ordering = true;
  d.known_unsupported_cases = {"octree", "support_contact_depth"};

  EXPECT_EQ(d.engine_id, "direct_roboplan_robot_only_compare");
  EXPECT_EQ(d.thread_safety,
            CollisionEngineThreadSafetyClass::kImmutableModelWorkerData);
  EXPECT_EQ(d.supported_geometry_kinds.size(), 2u);
}

}  // namespace
}  // namespace manyforge::core
