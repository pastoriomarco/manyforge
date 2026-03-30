#include <gtest/gtest.h>
#include <manyforge_core/cache/trajectory_cache.hpp>

namespace manyforge::core {
namespace {

TrajectoryDTO make_trajectory(const std::string& label) {
  TrajectoryDTO t;
  t.joint_names = {label};
  TrajectoryWaypointDTO wp;
  wp.time_from_start_s = 0.0;
  wp.positions_rad = {1.0, 2.0};
  t.waypoints.push_back(wp);
  return t;
}

class TrajectoryCacheTest : public ::testing::Test {
 protected:
  TrajectoryCache cache{CacheConfig{3}};  // max 3 entries
};

// --- Store and lookup ---
TEST_F(TrajectoryCacheTest, StoreAndLookup) {
  cache.store("move_1", make_trajectory("t1"));
  auto result = cache.lookup("move_1");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->joint_names[0], "t1");
  EXPECT_EQ(result->waypoints.size(), 1u);
}

TEST_F(TrajectoryCacheTest, LookupMiss_ReturnsNullopt) {
  auto result = cache.lookup("nonexistent");
  EXPECT_FALSE(result.has_value());
}

// --- Overwrite ---
TEST_F(TrajectoryCacheTest, StoreOverwritesExisting) {
  cache.store("move_1", make_trajectory("v1"));
  cache.store("move_1", make_trajectory("v2"));
  auto result = cache.lookup("move_1");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->joint_names[0], "v2");
  EXPECT_EQ(cache.size(), 1u);
}

// --- Invalidate ---
TEST_F(TrajectoryCacheTest, InvalidateSpecificMoveId) {
  cache.store("move_1", make_trajectory("t1"));
  cache.store("move_2", make_trajectory("t2"));
  cache.invalidate("move_1");
  EXPECT_FALSE(cache.lookup("move_1").has_value());
  EXPECT_TRUE(cache.lookup("move_2").has_value());
  EXPECT_EQ(cache.size(), 1u);
}

TEST_F(TrajectoryCacheTest, InvalidateNonExistent_NoOp) {
  cache.store("move_1", make_trajectory("t1"));
  cache.invalidate("nonexistent");
  EXPECT_EQ(cache.size(), 1u);
}

TEST_F(TrajectoryCacheTest, InvalidateAll) {
  cache.store("move_1", make_trajectory("t1"));
  cache.store("move_2", make_trajectory("t2"));
  cache.invalidate_all();
  EXPECT_EQ(cache.size(), 0u);
  EXPECT_FALSE(cache.lookup("move_1").has_value());
  EXPECT_FALSE(cache.lookup("move_2").has_value());
}

// --- Multiple independent entries ---
TEST_F(TrajectoryCacheTest, MultipleMovesIndependent) {
  cache.store("move_1", make_trajectory("t1"));
  cache.store("move_2", make_trajectory("t2"));
  cache.store("move_3", make_trajectory("t3"));

  EXPECT_EQ(cache.size(), 3u);
  EXPECT_EQ(cache.lookup("move_1")->joint_names[0], "t1");
  EXPECT_EQ(cache.lookup("move_2")->joint_names[0], "t2");
  EXPECT_EQ(cache.lookup("move_3")->joint_names[0], "t3");
}

// --- Size and capacity ---
TEST_F(TrajectoryCacheTest, SizeCorrect) {
  EXPECT_EQ(cache.size(), 0u);
  cache.store("a", make_trajectory("a"));
  EXPECT_EQ(cache.size(), 1u);
  cache.store("b", make_trajectory("b"));
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
  cache.store("move_1", make_trajectory("t1"));
  cache.store("move_2", make_trajectory("t2"));
  cache.store("move_3", make_trajectory("t3"));
  EXPECT_EQ(cache.size(), 3u);

  // 4th entry evicts oldest (move_1).
  cache.store("move_4", make_trajectory("t4"));
  EXPECT_EQ(cache.size(), 3u);
  EXPECT_FALSE(cache.lookup("move_1").has_value());
  EXPECT_TRUE(cache.lookup("move_2").has_value());
  EXPECT_TRUE(cache.lookup("move_3").has_value());
  EXPECT_TRUE(cache.lookup("move_4").has_value());
}

TEST_F(TrajectoryCacheTest, OverwriteDoesNotEvict) {
  cache.store("move_1", make_trajectory("t1"));
  cache.store("move_2", make_trajectory("t2"));
  cache.store("move_3", make_trajectory("t3"));

  // Overwrite move_1 — should not evict anything.
  cache.store("move_1", make_trajectory("t1_updated"));
  EXPECT_EQ(cache.size(), 3u);
  EXPECT_EQ(cache.lookup("move_1")->joint_names[0], "t1_updated");
  EXPECT_TRUE(cache.lookup("move_2").has_value());
  EXPECT_TRUE(cache.lookup("move_3").has_value());
}

// --- Empty trajectory ---
TEST_F(TrajectoryCacheTest, StoreEmptyTrajectory) {
  TrajectoryDTO empty;
  EXPECT_TRUE(empty.empty());
  cache.store("move_empty", empty);
  auto result = cache.lookup("move_empty");
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->empty());
}

}  // namespace
}  // namespace manyforge::core
