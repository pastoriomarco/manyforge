#include <gtest/gtest.h>
#include <manyforge_core/cache/trajectory_cache.hpp>
#include <manyforge_core/time/fake_clock.hpp>

namespace manyforge::core {
namespace {

class TrajectoryCacheTest : public ::testing::Test {
 protected:
  FakeClock clock_;

  CacheKey make_key(uint64_t req = 1, uint64_t start = 1,
                    uint64_t scene = 1,
                    const std::string& planner = "ompl",
                    uint64_t params = 1) {
    return {req, start, scene, planner, params};
  }

  TrajectoryDTO make_trajectory(const std::string& planner = "ompl",
                                uint64_t scene = 1) {
    TrajectoryDTO t;
    t.planner_backend_id = planner;
    t.scene_version = scene;
    t.joint_names = {"j1", "j2"};
    TrajectoryPoint p;
    p.positions = {0.1, 0.2};
    p.velocities = {0.0, 0.0};
    p.time_from_start_ns = 0;
    t.points.push_back(p);
    return t;
  }
};

TEST_F(TrajectoryCacheTest, StoreAndLookup) {
  TrajectoryCache cache(clock_, {10, 0});
  auto key = make_key();
  cache.store(key, make_trajectory());

  auto result = cache.lookup(key);
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.value().planner_backend_id, "ompl");
  EXPECT_EQ(result.value().points.size(), 1u);
}

TEST_F(TrajectoryCacheTest, CacheMissOnEmpty) {
  TrajectoryCache cache(clock_, {10, 0});
  auto result = cache.lookup(make_key());
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error(), ErrorCode::kCacheMiss);
}

TEST_F(TrajectoryCacheTest, CacheMissWrongKey) {
  TrajectoryCache cache(clock_, {10, 0});
  cache.store(make_key(1), make_trajectory());

  auto result = cache.lookup(make_key(2));
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error(), ErrorCode::kCacheMiss);
}

TEST_F(TrajectoryCacheTest, BoundsEnforced) {
  TrajectoryCache cache(clock_, {3, 0});
  cache.store(make_key(1), make_trajectory());
  cache.store(make_key(2), make_trajectory());
  cache.store(make_key(3), make_trajectory());
  EXPECT_EQ(cache.size(), 3u);

  // Adding a 4th should evict the LRU (key 1)
  cache.store(make_key(4), make_trajectory());
  EXPECT_EQ(cache.size(), 3u);
  EXPECT_FALSE(cache.lookup(make_key(1)).ok());
  EXPECT_TRUE(cache.lookup(make_key(4)).ok());
}

TEST_F(TrajectoryCacheTest, TTLExpiration) {
  TrajectoryCache cache(clock_, {10, 1000});  // 1000ns TTL
  clock_.set(0);
  cache.store(make_key(), make_trajectory());

  // Still valid
  clock_.set(999);
  EXPECT_TRUE(cache.lookup(make_key()).ok());

  // Expired
  clock_.set(1001);
  EXPECT_FALSE(cache.lookup(make_key()).ok());
  EXPECT_EQ(cache.size(), 0u);
}

TEST_F(TrajectoryCacheTest, InvalidateForScene) {
  TrajectoryCache cache(clock_, {10, 0});
  cache.store(make_key(1, 1, 1), make_trajectory("ompl", 1));
  cache.store(make_key(2, 1, 1), make_trajectory("ompl", 1));
  cache.store(make_key(3, 1, 2), make_trajectory("ompl", 2));
  EXPECT_EQ(cache.size(), 3u);

  // Keep only entries with scene version 2
  cache.invalidate_for_scene(2);
  EXPECT_EQ(cache.size(), 1u);
  EXPECT_FALSE(cache.lookup(make_key(1, 1, 1)).ok());
  EXPECT_TRUE(cache.lookup(make_key(3, 1, 2)).ok());
}

TEST_F(TrajectoryCacheTest, InvalidateAll) {
  TrajectoryCache cache(clock_, {10, 0});
  cache.store(make_key(1), make_trajectory());
  cache.store(make_key(2), make_trajectory());
  EXPECT_EQ(cache.size(), 2u);

  cache.invalidate_all();
  EXPECT_EQ(cache.size(), 0u);
}

TEST_F(TrajectoryCacheTest, LRUEviction) {
  TrajectoryCache cache(clock_, {3, 0});
  cache.store(make_key(1), make_trajectory());
  cache.store(make_key(2), make_trajectory());
  cache.store(make_key(3), make_trajectory());

  // Access key 1 to make it most recently used
  cache.lookup(make_key(1));

  // Insert key 4 — should evict key 2 (least recently used)
  cache.store(make_key(4), make_trajectory());
  EXPECT_EQ(cache.size(), 3u);
  EXPECT_TRUE(cache.lookup(make_key(1)).ok());
  EXPECT_FALSE(cache.lookup(make_key(2)).ok());
  EXPECT_TRUE(cache.lookup(make_key(3)).ok());
  EXPECT_TRUE(cache.lookup(make_key(4)).ok());
}

TEST_F(TrajectoryCacheTest, UpdateExistingEntry) {
  TrajectoryCache cache(clock_, {10, 0});
  auto key = make_key();
  cache.store(key, make_trajectory("planner_a"));
  cache.store(key, make_trajectory("planner_b"));

  EXPECT_EQ(cache.size(), 1u);
  auto result = cache.lookup(key);
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.value().planner_backend_id, "planner_b");
}

TEST_F(TrajectoryCacheTest, SizeAndCapacity) {
  TrajectoryCache cache(clock_, {5, 0});
  EXPECT_EQ(cache.size(), 0u);
  EXPECT_EQ(cache.capacity(), 5u);

  cache.store(make_key(1), make_trajectory());
  EXPECT_EQ(cache.size(), 1u);
}

}  // namespace
}  // namespace manyforge::core
