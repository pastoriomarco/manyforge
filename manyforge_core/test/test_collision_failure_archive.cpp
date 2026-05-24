// C-arch unit tests for CollisionFailureArchive: bounded ring buffer,
// sink callback, capacity overflow drops, recent() newest-first ordering.

#include <manyforge_core/collision/collision_failure_archive.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace manyforge::core {
namespace {

CollisionFailureArchiveEntry make_entry(const std::string& id,
                                        std::int64_t emitted_at_ns) {
  CollisionFailureArchiveEntry e;
  e.entry_id = id;
  e.emitted_at_ns = emitted_at_ns;
  e.engine_id = "test-engine";
  e.engine_version = "v0";
  e.collision_world_revision = CollisionWorldRevision{1, "rev-1"};
  e.failure_kind = CollisionFailureKind::kBlockingCollision;
  e.decision_action = CollisionDecisionAction::kReject;
  e.decision_severity = CollisionDecisionSeverity::kBlock;
  e.trajectory_waypoint_count = 16;
  return e;
}

TEST(CollisionFailureArchive, StoresAndReturnsEntriesNewestFirst) {
  CollisionFailureArchive archive(8);
  archive.push(make_entry("a", 1));
  archive.push(make_entry("b", 2));
  archive.push(make_entry("c", 3));

  auto recent = archive.recent();
  ASSERT_EQ(recent.size(), 3u);
  EXPECT_EQ(recent[0].entry_id, "c");
  EXPECT_EQ(recent[1].entry_id, "b");
  EXPECT_EQ(recent[2].entry_id, "a");
}

TEST(CollisionFailureArchive, DropsOldestWhenOverCapacity) {
  CollisionFailureArchive archive(2);
  archive.push(make_entry("a", 1));
  archive.push(make_entry("b", 2));
  archive.push(make_entry("c", 3));
  archive.push(make_entry("d", 4));

  auto recent = archive.recent();
  ASSERT_EQ(recent.size(), 2u);
  EXPECT_EQ(recent[0].entry_id, "d");
  EXPECT_EQ(recent[1].entry_id, "c");
  EXPECT_EQ(archive.total_pushed(), 4u);
  EXPECT_EQ(archive.total_dropped(), 2u);
}

TEST(CollisionFailureArchive, RecentRespectsLimit) {
  CollisionFailureArchive archive(8);
  for (int i = 0; i < 5; ++i) {
    archive.push(make_entry(std::to_string(i), i));
  }
  auto two = archive.recent(2);
  ASSERT_EQ(two.size(), 2u);
  EXPECT_EQ(two[0].entry_id, "4");
  EXPECT_EQ(two[1].entry_id, "3");
}

TEST(CollisionFailureArchive, SinkFiresOnPush) {
  CollisionFailureArchive archive(8);
  std::vector<std::string> seen;
  archive.set_sink([&seen](const CollisionFailureArchiveEntry& e) {
    seen.push_back(e.entry_id);
  });
  archive.push(make_entry("a", 1));
  archive.push(make_entry("b", 2));
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_EQ(seen[0], "a");
  EXPECT_EQ(seen[1], "b");
}

TEST(CollisionFailureArchive, ZeroCapacityStillFiresSink) {
  CollisionFailureArchive archive(0);
  std::vector<std::string> seen;
  archive.set_sink([&seen](const CollisionFailureArchiveEntry& e) {
    seen.push_back(e.entry_id);
  });
  archive.push(make_entry("a", 1));
  archive.push(make_entry("b", 2));
  EXPECT_TRUE(archive.recent().empty());
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_EQ(seen[0], "a");
  EXPECT_EQ(seen[1], "b");
  EXPECT_EQ(archive.total_pushed(), 2u);
  EXPECT_EQ(archive.total_dropped(), 2u);
}

TEST(CollisionFailureArchive, SinkMayReadArchiveWithoutDeadlock) {
  CollisionFailureArchive archive(8);
  std::vector<std::size_t> retained_counts_seen_by_sink;
  archive.set_sink([&archive, &retained_counts_seen_by_sink](
                       const CollisionFailureArchiveEntry&) {
    retained_counts_seen_by_sink.push_back(archive.recent().size());
  });

  archive.push(make_entry("a", 1));
  archive.push(make_entry("b", 2));

  ASSERT_EQ(retained_counts_seen_by_sink.size(), 2u);
  EXPECT_EQ(retained_counts_seen_by_sink[0], 1u);
  EXPECT_EQ(retained_counts_seen_by_sink[1], 2u);
}

TEST(CollisionFailureArchive, ClearDropsEntriesPreservesCounters) {
  CollisionFailureArchive archive(8);
  archive.push(make_entry("a", 1));
  archive.push(make_entry("b", 2));
  archive.clear();
  EXPECT_TRUE(archive.recent().empty());
  EXPECT_EQ(archive.total_pushed(), 2u);
}

TEST(CollisionFailureArchive, ConcurrentPushesRemainBoundedAndCounted) {
  constexpr int kThreads = 4;
  constexpr int kPerThread = 50;
  constexpr std::size_t kCapacity = 64;
  CollisionFailureArchive archive(kCapacity);

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&archive, t]() {
      for (int i = 0; i < kPerThread; ++i) {
        const std::string entry_id =
            std::string("t") + std::to_string(t) + "-" + std::to_string(i);
        archive.push(make_entry(entry_id, t * kPerThread + i));
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(archive.total_pushed(),
            static_cast<std::uint64_t>(kThreads * kPerThread));
  EXPECT_EQ(archive.recent().size(), kCapacity);
  EXPECT_EQ(archive.total_dropped(),
            static_cast<std::uint64_t>(kThreads * kPerThread - kCapacity));
}

}  // namespace
}  // namespace manyforge::core
